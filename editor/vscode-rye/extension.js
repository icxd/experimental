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

function log(message) {
  const line = `[${new Date().toISOString()}] ${message}`;
  output?.appendLine(line);
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

  try {
    await stopping.stop();
    log("Language server stopped.");
  } catch (error) {
    log(`Language server stop failed: ${error}`);
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

  startInFlight = (async () => {
    let LanguageClient;
    let TransportKind;

    try {
      ({ LanguageClient, TransportKind } = require("vscode-languageclient/node"));
    } catch (error) {
      const message =
        "Failed to load vscode-languageclient. Run `npm install --omit=dev` in editor/vscode-rye, then reload the window.";
      log(message);
      log(String(error));
      void vscode.window.showErrorMessage(`Rye: ${message}`);
      return;
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
      return;
    }

    await stopLanguageClient();

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

    client = newClient;
    bindOutputChannel(newClient);
    ensureDisposeHook(context);

    try {
      await newClient.start();
      log("Language server started.");
    } catch (error) {
      client = undefined;
      log(`Failed to start language server: ${error}`);
      void vscode.window.showErrorMessage(
        `Rye: failed to start language server. See Output → Rye for details.`
      );
    }
  })();

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
  await startLanguageClient(context);
  void vscode.window.showInformationMessage("Rye language server restarted.");
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
  await stopLanguageClient();
}

module.exports = { activate, deactivate };
