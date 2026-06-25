const vscode = require("vscode");
const path = require("path");
const fs = require("fs");

/** @type {import("vscode-languageclient/node").LanguageClient | undefined} */
let client;

/** @type {vscode.OutputChannel | undefined} */
let output;

/** @type {vscode.ExtensionContext | undefined} */
let extensionContext;

/** @type {Promise<void> | undefined} */
let startInFlight;

/** @type {boolean} */
let disposeHookRegistered = false;

const STOP_TIMEOUT_MS = 5000;
const RESTART_DELAY_MS = 200;

function log(message) {
  const line = `[${new Date().toISOString()}] ${message}`;
  output?.appendLine(line);
}

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function stateLabel(state) {
  const labels = {
    1: "Stopped",
    2: "Running",
    3: "Starting",
    4: "StartFailed",
    5: "Stopping",
  };
  return labels[state] ?? String(state);
}

/**
 * @param {string} value
 * @param {string} workspaceFolder
 * @param {string} documentDir
 */
function resolveConfigPath(value, workspaceFolder, documentDir) {
  if (!value || typeof value !== "string") return value;

  let resolved = value
    .replace(/\$\{workspaceFolder\}/g, workspaceFolder)
    .replace(/\$\{workspaceRoot\}/g, workspaceFolder)
    .replace(/\$\{fileDirname\}/g, documentDir);

  if (!path.isAbsolute(resolved) && workspaceFolder) {
    resolved = path.join(workspaceFolder, resolved);
  }

  return resolved;
}

/**
 * @param {vscode.WorkspaceConfiguration} config
 * @param {vscode.WorkspaceFolder | undefined} workspaceFolder
 */
function resolveRyePath(config, workspaceFolder) {
  const workspacePath = workspaceFolder?.uri.fsPath ?? "";
  const configured = config.get("compilerPath", "rye");
  const resolved = resolveConfigPath(configured, workspacePath, workspacePath);

  if (fs.existsSync(resolved)) return resolved;

  if (workspacePath) {
    const fallback = path.join(workspacePath, "build", "rye");
    if (fs.existsSync(fallback)) return fallback;
  }

  return resolved;
}

/**
 * @param {vscode.WorkspaceConfiguration} config
 * @param {vscode.WorkspaceFolder | undefined} workspaceFolder
 */
function buildImportPaths(config, workspaceFolder) {
  const workspacePath = workspaceFolder?.uri.fsPath ?? "";
  const importPaths = config.get("importPaths", []);
  const seen = new Set();
  const resolved = [];

  for (const importPath of importPaths) {
    const pathValue = resolveConfigPath(importPath, workspacePath, workspacePath);
    if (pathValue && !seen.has(pathValue)) {
      seen.add(pathValue);
      resolved.push(pathValue);
    }
  }

  if (workspacePath && !seen.has(workspacePath)) {
    resolved.push(workspacePath);
  }

  return resolved;
}

/**
 * @param {import("vscode-languageclient/node").LanguageClient} languageClient
 */
function bindOutputChannel(languageClient) {
  output = languageClient.outputChannel;
}

/**
 * @param {vscode.ExtensionContext} context
 */
function ensureDisposeHook(context) {
  if (disposeHookRegistered) return;
  disposeHookRegistered = true;
  context.subscriptions.push({
    dispose: () => {
      void stopLanguageClient();
    },
  });
}

async function stopLanguageClient() {
  if (!client) return;

  const stopping = client;
  client = undefined;
  log(`Stopping language server (state: ${stateLabel(stopping.state)})...`);

  try {
    if (stopping.state === 2 || stopping.state === 3 || stopping.state === 5) {
      await stopping.stop(STOP_TIMEOUT_MS);
    }
  } catch (error) {
    log(`Language server stop failed: ${error}`);
  }

  try {
    stopping.dispose();
  } catch (error) {
    log(`Language server dispose failed: ${error}`);
  }

  log("Language server stopped.");
}

/**
 * @param {vscode.ExtensionContext} context
 */
async function runStartLanguageClient(context) {
  let LanguageClient;
  let TransportKind;

  try {
    ({ LanguageClient, TransportKind } = require("vscode-languageclient/node"));
  } catch (error) {
    const message =
      "Failed to load vscode-languageclient. Run `npm install --omit=dev` in editor/vscode-rye, then reload the window.";
    log(message);
    log(String(error));
    throw new Error(message);
  }

  const workspaceFolder = vscode.workspace.workspaceFolders?.[0];
  const config = vscode.workspace.getConfiguration("rye", workspaceFolder?.uri);

  const serverPath = resolveRyePath(config, workspaceFolder);
  if (!fs.existsSync(serverPath)) {
    const message = `Rye compiler not found at: ${serverPath}`;
    log(message);
    void vscode.window.showErrorMessage(
      `${message}. Build with: cmake --build build --target rye`
    );
    throw new Error(message);
  }

  const importPaths = buildImportPaths(config, workspaceFolder);
  const workspacePath = workspaceFolder?.uri.fsPath ?? "";
  const cwd = workspacePath || path.dirname(serverPath);

  log(`Starting language server: ${serverPath} lsp`);
  log(`Working directory: ${cwd}`);
  log(`Import paths: ${importPaths.join(", ") || "(none)"}`);
  log(`LSP traffic log: ${path.join(cwd, ".rye", "lsp-traffic.log")}`);

  const serverOptions = {
    command: serverPath,
    args: ["lsp"],
    transport: TransportKind.stdio,
    options: {
      cwd,
      env: {
        ...process.env,
        ...(config.get("lspDebug", false) ? { RYE_LSP_DEBUG: "1" } : {}),
      },
    },
  };

  const clientOptions = {
    documentSelector: [{ language: "rye", scheme: "file" }],
    initializationOptions: {
      importPaths,
      workspaceRoot: workspacePath || cwd,
    },
    outputChannelName: "Rye",
    synchronize: {
      configurationSection: "rye",
    },
  };

  const newClient = new LanguageClient(
    "rye",
    "Rye Language Server",
    serverOptions,
    clientOptions
  );

  newClient.onDidChangeState((event) => {
    log(
      `Language client state: ${stateLabel(event.oldState)} -> ${stateLabel(event.newState)}`
    );
  });

  client = newClient;
  bindOutputChannel(newClient);
  ensureDisposeHook(context);
  context.subscriptions.push(newClient);

  try {
    await newClient.start();
    log("Language server started.");
  } catch (error) {
    client = undefined;
    log(`Failed to start language server: ${error}`);
    try {
      newClient.dispose();
    } catch {
      // ignore
    }
    void vscode.window.showErrorMessage(
      "Failed to start language server. See Output → Rye for details."
    );
    throw error;
  }
}

/**
 * @param {vscode.ExtensionContext} context
 */
async function startLanguageClient(context) {
  if (startInFlight) {
    await startInFlight;
    return;
  }

  startInFlight = runStartLanguageClient(context);

  try {
    await startInFlight;
  } finally {
    startInFlight = undefined;
  }
}

/**
 * @param {vscode.ExtensionContext} context
 */
async function restartLanguageServer(context) {
  log("Restarting language server...");

  if (startInFlight) {
    try {
      await startInFlight;
    } catch {
      // A failed start should not block restart.
    }
  }

  startInFlight = undefined;
  await stopLanguageClient();
  await delay(RESTART_DELAY_MS);

  startInFlight = runStartLanguageClient(context);

  try {
    await startInFlight;
    if (!client) {
      throw new Error("language client is not running after restart");
    }
    log("Language server restart complete.");
    void vscode.window.showInformationMessage("Language server restarted.");
  } catch (error) {
    log(`Restart failed: ${error}`);
    void vscode.window.showErrorMessage(
      "Failed to restart language server. See Output → Rye for details."
    );
  } finally {
    startInFlight = undefined;
  }
}

/**
 * @param {vscode.ExtensionContext} context
 */
function activate(context) {
  extensionContext = context;
  output = vscode.window.createOutputChannel("Rye");
  context.subscriptions.push(output);

  log("Rye extension activated.");
  void startLanguageClient(context);

  context.subscriptions.push(
    vscode.commands.registerCommand("rye.restartLanguageServer", () => {
      if (extensionContext) void restartLanguageServer(extensionContext);
    }),
    vscode.commands.registerCommand("rye.showOutput", () => {
      output?.show(true);
    })
  );
}

async function deactivate() {
  log("Rye extension deactivating.");
  if (startInFlight) {
    try {
      await startInFlight;
    } catch {
      // ignore
    }
  }
  startInFlight = undefined;
  await stopLanguageClient();
}

module.exports = { activate, deactivate };
