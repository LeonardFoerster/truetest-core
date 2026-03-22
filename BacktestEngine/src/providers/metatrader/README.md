# MetaTrader Provider

Not yet implemented. When built, this directory will contain:

- `metatrader_provider.h/.cpp` — implements `IProvider`
- `metatrader_transport.h/.cpp` — implements `IDataTransport` (EA bridge via named pipe/socket)
- `metatrader_parser.h` — implements `IDataParser<provider::event>`
- `metatrader_executor.h/.cpp` — implements `IExecutionAdapter` (EA order relay)
