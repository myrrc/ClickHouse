#pragma once

#include <Databases/IDatabase.h>
#include <Interpreters/IInterpreter.h>
#include <Parsers/New/ASTDeleteQuery.h>
#include <Parsers/IAST_fwd.h>

namespace DB
{
class Context;
using DatabaseAndTable = std::pair<DatabasePtr, StoragePtr>;
class AccessRightsElements;

class InterpreterDeleteQuery : public IInterpreter, WithMutableContext
{
public:
    InterpreterDeleteQuery(const ASTPtr & query_ptr_, ContextMutablePtr context_);

    /// Drop table or database.
    BlockIO execute() override;
};
}
