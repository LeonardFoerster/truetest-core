# Polymarket Provider

Not yet implemented. When built, this directory will contain:

- `polymarket_provider.h/.cpp` — implements `IProvider`
- `polymarket_transport.h/.cpp` — implements `IDataTransport` (WebSocket/API client)
- `polymarket_parser.h` — implements `IDataParser<provider::event>`
- `polymarket_executor.h/.cpp` — implements `IExecutionAdapter` (REST order submission)
