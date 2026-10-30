// Stdio MCP client for the UE plugin McpAutomationBridge 0.5.30 (ChiR24/Unreal_mcp) via unreal-engine-mcp-server 0.5.30.
// Setup (PC): copy this folder to %TEMP%\btl_mcp, `npm install` there (versions pinned in package.json; server must match the plugin).
// Usage: node btl_mcp_client.mjs [<tool> '<json args>']  -> prints the tool list, then the call result (first 3000 chars).
// Read-only use for assets (no modify/save/delete tools). Editor must be running with the bridge listening on 8091.
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
const t = new StdioClientTransport({ command: "node", args: ["node_modules/unreal-engine-mcp-server/dist/cli.js"], env: { ...process.env, MCP_AUTOMATION_HOST:"127.0.0.1", MCP_AUTOMATION_PORT:"8091" } });
const c = new Client({ name: "bongbot", version: "1.0" });
await c.connect(t);
const tools = await c.listTools();
console.log("TOOLS", tools.tools.length, tools.tools.map(x=>x.name).join(","));
const [name, argsJson] = process.argv.slice(2);
if (name) { const r = await c.callTool({ name, arguments: JSON.parse(argsJson||"{}") }); console.log(JSON.stringify(r).slice(0,3000)); }
await c.close(); process.exit(0);
