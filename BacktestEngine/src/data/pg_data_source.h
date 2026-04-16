#pragma once
#ifdef HAS_POSTGRESQL

#include "data_source.h"
#include "../utils/retry.h"
#include <optional>
#include <pqxx/pqxx>
#include <memory>

class PgDataSource : public IDataSource {
    std::optional<pqxx::connection> connection_;
public:
    pqxx::connection& establish_connection();
    void test_connection();
    bool load_data(std::shared_ptr<data_handler> handler) override;
};

#endif
