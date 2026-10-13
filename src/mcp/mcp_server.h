#pragma once

// MCP stdio server entry point (src/mcp/mcp_server.cpp).
// Owns one Memory + one InstanceStore, speaks newline-delimited JSON-RPC 2.0
// on stdin/stdout. Declared here so app/main.cpp can dispatch `--mcp` to it
// without pulling any MCP details into the normal UI flow.
int RunMcpServer();
