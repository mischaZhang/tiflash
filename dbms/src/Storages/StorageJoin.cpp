// Copyright 2022 PingCAP, Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <Common/typeid_cast.h>
#include <Interpreters/Join.h>
#include <Parsers/ASTIdentifier.h>
#include <Poco/String.h> /// toLower
#include <Storages/StorageFactory.h>
#include <Storages/StorageJoin.h>


namespace DB
{
namespace ErrorCodes
{
extern const int NO_SUCH_COLUMN_IN_TABLE;
extern const int INCOMPATIBLE_TYPE_OF_JOIN;
extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
extern const int BAD_ARGUMENTS;
} // namespace ErrorCodes


StorageJoin::StorageJoin(
    const String & path_,
    const String & name_,
    const Names & key_names_,
    ASTTableJoin::Kind kind_,
    ASTTableJoin::Strictness strictness_,
    const ColumnsDescription & columns_)
    : StorageSetOrJoinBase{path_, name_, columns_}
    , key_names(key_names_)
    , kind(kind_)
    , strictness(strictness_)
{
    for (const auto & key : key_names)
        if (!getColumns().hasPhysical(key))
            throw Exception{
                "Key column (" + key + ") does not exist in table declaration.",
                ErrorCodes::NO_SUCH_COLUMN_IN_TABLE};

    /// NOTE StorageJoin doesn't use join_use_nulls setting.

    join = std::make_shared<Join>(key_names, key_names, false /* use_nulls */, SizeLimits(), kind, strictness, /*req_id=*/"");
    join->init(getSampleBlock().sortColumns());
    restore();
}


void StorageJoin::assertCompatible(ASTTableJoin::Kind kind_, ASTTableJoin::Strictness strictness_) const
{
    /// NOTE Could be more loose.
    if (!(kind == kind_ && strictness == strictness_))
        throw Exception("Table " + table_name + " has incompatible type of JOIN.", ErrorCodes::INCOMPATIBLE_TYPE_OF_JOIN);
}


void StorageJoin::insertBlock(const Block & block)
{
    join->insertFromBlock(block);
}
size_t StorageJoin::getSize() const
{
    return join->getTotalRowCount();
};


void registerStorageJoin(StorageFactory & factory)
{
    factory.registerStorage("Join", [](const StorageFactory::Arguments & args) {
        /// Join(ANY, LEFT, k1, k2, ...)

        ASTs & engine_args = args.engine_args;

        if (engine_args.size() < 3)
            throw Exception(
                "Storage Join requires at least 3 parameters: Join(ANY|ALL, LEFT|INNER, keys...).",
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        const auto * strictness_id = typeid_cast<const ASTIdentifier *>(engine_args[0].get());
        if (!strictness_id)
            throw Exception("First parameter of storage Join must be ANY or ALL (without quotes).", ErrorCodes::BAD_ARGUMENTS);

        const String strictness_str = Poco::toLower(strictness_id->name);
        ASTTableJoin::Strictness strictness;
        if (strictness_str == "any")
            strictness = ASTTableJoin::Strictness::Any;
        else if (strictness_str == "all")
            strictness = ASTTableJoin::Strictness::All;
        else
            throw Exception("First parameter of storage Join must be ANY or ALL (without quotes).", ErrorCodes::BAD_ARGUMENTS);

        const auto * kind_id = typeid_cast<const ASTIdentifier *>(engine_args[1].get());
        if (!kind_id)
            throw Exception("Second parameter of storage Join must be LEFT or INNER (without quotes).", ErrorCodes::BAD_ARGUMENTS);

        const String kind_str = Poco::toLower(kind_id->name);
        ASTTableJoin::Kind kind;
        if (kind_str == "left")
            kind = ASTTableJoin::Kind::Left;
        else if (kind_str == "inner")
            kind = ASTTableJoin::Kind::Inner;
        else if (kind_str == "right")
            kind = ASTTableJoin::Kind::Right;
        else if (kind_str == "full")
            kind = ASTTableJoin::Kind::Full;
        else
            throw Exception("Second parameter of storage Join must be LEFT or INNER or RIGHT or FULL (without quotes).", ErrorCodes::BAD_ARGUMENTS);

        Names key_names;
        key_names.reserve(engine_args.size() - 2);
        for (size_t i = 2, size = engine_args.size(); i < size; ++i)
        {
            const auto * key = typeid_cast<const ASTIdentifier *>(engine_args[i].get());
            if (!key)
                throw Exception("Parameter №" + toString(i + 1) + " of storage Join don't look like column name.", ErrorCodes::BAD_ARGUMENTS);

            key_names.push_back(key->name);
        }

        return StorageJoin::create(
            args.data_path,
            args.table_name,
            key_names,
            kind,
            strictness,
            args.columns);
    });
}

} // namespace DB
