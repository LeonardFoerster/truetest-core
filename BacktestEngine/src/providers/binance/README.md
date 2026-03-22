# Binance Provider

Not yet implemented. When built, this directory will contain:

- `binance_provider.h/.cpp` — implements `IProvider`
- `binance_transport.h/.cpp` — implements `IDataTransport` (WebSocket client)
- `binance_parser.h` — implements `IDataParser<provider::event>`
- `binance_executor.h/.cpp` — implements `IExecutionAdapter` (REST/WS order submission)
