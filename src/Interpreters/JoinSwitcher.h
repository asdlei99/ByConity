/*
 * Copyright 2016-2023 ClickHouse, Inc.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/*
 * This file may have been modified by Bytedance Ltd. and/or its affiliates (“ Bytedance's Modifications”).
 * All Bytedance's Modifications are Copyright (2023) Bytedance Ltd. and/or its affiliates.
 */

#pragma once

#include <mutex>

#include <Core/Block.h>
#include <Interpreters/IJoin.h>
#include <Interpreters/TableJoin.h>
#include <DataStreams/IBlockInputStream.h>


namespace DB
{

/// Used when setting 'join_algorithm' set to JoinAlgorithm::AUTO.
/// Starts JOIN with join-in-memory algorithm and switches to join-on-disk on the fly if there's no memory to place right table.
/// Current join-in-memory and join-on-disk are JoinAlgorithm::HASH and JoinAlgorithm::PARTIAL_MERGE joins respectively.
class JoinSwitcher : public IJoin
{
public:
    JoinSwitcher(std::shared_ptr<TableJoin> table_join_, const Block & right_sample_block_);

    JoinType getType() const override { return JoinType::Switcher; }

    const TableJoin & getTableJoin() const override { return *table_join; }
    TableJoin & getTableJoin() override { return *table_join; }

    /// Add block of data from right hand of JOIN into current join object.
    /// If join-in-memory memory limit exceeded switches to join-on-disk and continue with it.
    /// @returns false, if join-on-disk disk limit exceeded
    bool addJoinedBlock(const Block & block, bool check_limits) override;

    void joinBlock(Block & block, std::shared_ptr<ExtraBlock> & not_processed) override
    {
        join->joinBlock(block, not_processed);
    }

    const Block & getTotals() const override
    {
        return join->getTotals();
    }

    void setTotals(const Block & block) override
    {
        join->setTotals(block);
    }

    size_t getTotalRowCount() const override
    {
        return join->getTotalRowCount();
    }

    size_t getTotalByteCount() const override
    {
        return join->getTotalByteCount();
    }

    bool alwaysReturnsEmptySet() const override
    {
        return join->alwaysReturnsEmptySet();
    }

    BlockInputStreamPtr createStreamWithNonJoinedRows(const Block & block, UInt64 max_block_size) const override
    {
        return join->createStreamWithNonJoinedRows(block, max_block_size);
    }

    void tryBuildRuntimeFilters(size_t total_rows) const override
    {
        join->tryBuildRuntimeFilters(total_rows);
    }

private:
    JoinPtr join;
    SizeLimits limits;
    bool switched;
    mutable std::mutex switch_mutex;
    std::shared_ptr<TableJoin> table_join;
    const Block right_sample_block;

    /// Change join-in-memory to join-on-disk moving right hand JOIN data from one to another.
    /// Throws an error if join-on-disk do not support JOIN kind or strictness.
    void switchJoin();
};


/// Creates NonJoinedBlockInputStream on the first read. Allows to swap join algo before it.
class LazyNonJoinedBlockInputStream : public IBlockInputStream
{
public:
    LazyNonJoinedBlockInputStream(const IJoin & join_, const Block & block, UInt64 max_block_size_)
        : join(join_)
        , result_sample_block(block)
        , max_block_size(max_block_size_)
    {}

    String getName() const override { return "LazyNonMergeJoined"; }
    Block getHeader() const override { return result_sample_block; }

protected:
    Block readImpl() override
    {
        if (!stream)
        {
            stream = join.createStreamWithNonJoinedRows(result_sample_block, max_block_size);
            if (!stream)
                return {};
        }

        return stream->read();
    }

private:
    BlockInputStreamPtr stream;
    const IJoin & join;
    Block result_sample_block;
    UInt64 max_block_size;
};

}
