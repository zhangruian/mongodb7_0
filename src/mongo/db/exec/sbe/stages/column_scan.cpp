/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/stages/column_scan.h"

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/values/columnar.h"

namespace mongo {
namespace sbe {
ColumnScanStage::ColumnScanStage(UUID collectionUuid,
                                 StringData columnIndexName,
                                 value::SlotVector fieldSlots,
                                 std::vector<std::string> paths,
                                 boost::optional<value::SlotId> recordSlot,
                                 boost::optional<value::SlotId> recordIdSlot,
                                 std::unique_ptr<EExpression> recordExpr,
                                 std::vector<std::unique_ptr<EExpression>> pathExprs,
                                 value::SlotId rowStoreSlot,
                                 PlanYieldPolicy* yieldPolicy,
                                 PlanNodeId nodeId)
    : PlanStage("columnscan"_sd, yieldPolicy, nodeId),
      _collUuid(collectionUuid),
      _columnIndexName(columnIndexName),
      _fieldSlots(std::move(fieldSlots)),
      _paths(std::move(paths)),
      _recordSlot(recordSlot),
      _recordIdSlot(recordIdSlot),
      _recordExpr(std::move(recordExpr)),
      _pathExprs(std::move(pathExprs)),
      _rowStoreSlot(rowStoreSlot) {
    invariant(_fieldSlots.size() == _paths.size());
    invariant(_fieldSlots.size() == _pathExprs.size());
}

std::unique_ptr<PlanStage> ColumnScanStage::clone() const {
    std::vector<std::unique_ptr<EExpression>> pathExprs;
    for (auto& expr : _pathExprs) {
        pathExprs.emplace_back(expr->clone());
    }
    return std::make_unique<ColumnScanStage>(_collUuid,
                                             _columnIndexName,
                                             _fieldSlots,
                                             _paths,
                                             _recordSlot,
                                             _recordIdSlot,
                                             _recordExpr ? _recordExpr->clone() : nullptr,
                                             std::move(pathExprs),
                                             _rowStoreSlot,
                                             _yieldPolicy,
                                             _commonStats.nodeId);
}

void ColumnScanStage::prepare(CompileCtx& ctx) {
    _outputFields.resize(_fieldSlots.size());

    for (size_t idx = 0; idx < _outputFields.size(); ++idx) {
        auto [it, inserted] = _outputFieldsMap.emplace(_fieldSlots[idx], &_outputFields[idx]);
        uassert(6298601, str::stream() << "duplicate slot: " << _fieldSlots[idx], inserted);
    }

    if (_recordSlot) {
        _recordAccessor = std::make_unique<value::OwnedValueAccessor>();
    }
    if (_recordIdSlot) {
        _recordIdAccessor = std::make_unique<value::OwnedValueAccessor>();
    }

    _rowStoreAccessor = std::make_unique<value::OwnedValueAccessor>();
    if (_recordExpr) {
        ctx.root = this;
        _recordExprCode = _recordExpr->compile(ctx);
    }
    for (auto& expr : _pathExprs) {
        ctx.root = this;
        _pathExprsCode.emplace_back(expr->compile(ctx));
    }

    tassert(6298602, "'_coll' should not be initialized prior to 'acquireCollection()'", !_coll);
    std::tie(_coll, _collName, _catalogEpoch) = acquireCollection(_opCtx, _collUuid);
}

value::SlotAccessor* ColumnScanStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_recordSlot && slot == *_recordSlot) {
        return _recordAccessor.get();
    }

    if (_recordIdSlot && slot == *_recordIdSlot) {
        return _recordIdAccessor.get();
    }

    if (auto it = _outputFieldsMap.find(slot); it != _outputFieldsMap.end()) {
        return it->second;
    }

    if (_rowStoreSlot == slot) {
        return _rowStoreAccessor.get();
    }
    return ctx.getAccessor(slot);
}

void ColumnScanStage::doSaveState(bool relinquishCursor) {
    if (_rowStoreCursor && relinquishCursor) {
        _rowStoreCursor->save();
    }

    if (_rowStoreCursor) {
        _rowStoreCursor->setSaveStorageCursorOnDetachFromOperationContext(!relinquishCursor);
    }

    for (auto& cursor : _columnCursors) {
        cursor.cursor->save();
    }
    for (auto& [path, cursor] : _parentPathCursors) {
        cursor->save();
    }

    _coll.reset();
}

void ColumnScanStage::doRestoreState(bool relinquishCursor) {
    invariant(_opCtx);
    invariant(!_coll);

    // If this stage has not been prepared, then yield recovery is a no-op.
    if (!_collName) {
        return;
    }

    tassert(6298603, "Catalog epoch should be initialized", _catalogEpoch);
    _coll = restoreCollection(_opCtx, *_collName, _collUuid, *_catalogEpoch);

    if (_rowStoreCursor) {
        if (relinquishCursor) {
            const bool couldRestore = _rowStoreCursor->restore();
            invariant(couldRestore);
        }
    }

    for (auto& cursor : _columnCursors) {
        cursor.cursor->restore();
    }
    for (auto& [path, cursor] : _parentPathCursors) {
        cursor->restore();
    }
}

void ColumnScanStage::doDetachFromOperationContext() {
    if (_rowStoreCursor) {
        _rowStoreCursor->detachFromOperationContext();
    }
    for (auto& cursor : _columnCursors) {
        cursor.cursor->detachFromOperationContext();
    }
    for (auto& [path, cursor] : _parentPathCursors) {
        cursor->detachFromOperationContext();
    }
}

void ColumnScanStage::doAttachToOperationContext(OperationContext* opCtx) {
    if (_rowStoreCursor) {
        _rowStoreCursor->reattachToOperationContext(opCtx);
    }
    for (auto& cursor : _columnCursors) {
        cursor.cursor->reattachToOperationContext(opCtx);
    }
    for (auto& [path, cursor] : _parentPathCursors) {
        cursor->reattachToOperationContext(opCtx);
    }
}

void ColumnScanStage::doDetachFromTrialRunTracker() {
    _tracker = nullptr;
}

PlanStage::TrialRunTrackerAttachResultMask ColumnScanStage::doAttachToTrialRunTracker(
    TrialRunTracker* tracker, TrialRunTrackerAttachResultMask childrenAttachResult) {
    _tracker = tracker;
    return childrenAttachResult | TrialRunTrackerAttachResultFlags::AttachedToStreamingStage;
}

void ColumnScanStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    invariant(_opCtx);

    if (_open) {
        tassert(6298604, "reopened ColumnScanStage but reOpen=false", reOpen);
        tassert(6298605, "ColumnScanStage is open but _coll is not null", _coll);
        tassert(6298606, "ColumnScanStage is open but don't have _rowStoreCursor", _rowStoreCursor);
    } else {
        tassert(6298607, "first open to ColumnScanStage but reOpen=true", !reOpen);
        if (!_coll) {
            // We're being opened after 'close()'. We need to re-acquire '_coll' in this case and
            // make some validity checks (the collection has not been dropped, renamed, etc.).
            tassert(
                6298608, "ColumnScanStage is not open but have _rowStoreCursor", !_rowStoreCursor);
            tassert(6298609, "Collection name should be initialized", _collName);
            tassert(6298610, "Catalog epoch should be initialized", _catalogEpoch);
            _coll = restoreCollection(_opCtx, *_collName, _collUuid, *_catalogEpoch);
        }
    }

    if (!_rowStoreCursor) {
        _rowStoreCursor = _coll->getCursor(_opCtx, true);
    }

    if (_columnCursors.empty()) {
        // We fake the special Row ID column by just using _id, for now.
        _columnCursors.push_back(ColumnCursor{
            std::make_unique<FakeCursorForPath>("_id", _coll->getCursor(_opCtx, true)),
            boost::none,
            false /* add to document */
        });

        for (auto&& path : _paths) {
            _columnCursors.push_back(ColumnCursor{
                std::make_unique<FakeCursorForPath>(path, _coll->getCursor(_opCtx, true)),
                boost::none,
                true /* add to document */
            });
        }
    }

    _open = true;
    _firstGetNext = true;
}

void ColumnScanStage::readParentsIntoObj(StringData path,
                                         value::Object* outObj,
                                         StringDataSet* pathsReadSetOut,
                                         bool first) {
    auto parent = ColumnStore::getParentPath(path);

    // If a top-level path doesn't exist, it just doesn't exist. It can't exist in some places
    // within a document but not others. No further inspection is necessary.

    if (!parent) {
        return;
    }

    if (pathsReadSetOut->contains(*parent)) {
        // We've already read the parent in, so skip it.
        return;
    }

    // Create the parent path cursor if necessary.

    // First we try to emplace a nullptr, so that we avoid creating the cursor when we don't have
    // to.
    auto [it, inserted] = _parentPathCursors.try_emplace(*parent, nullptr);

    // If we inserted a new entry, replace the null with an actual cursor.
    if (inserted) {
        invariant(it->second == nullptr);
        it->second = std::make_unique<FakeCursorForPath>(*parent, _coll->getCursor(_opCtx, true));
    }

    auto optCell = it->second->seekExact(_recordId);
    pathsReadSetOut->insert(*parent);

    if (!optCell || optCell->isSparse) {
        // We need this cell's parent too.
        readParentsIntoObj(*parent, outObj, pathsReadSetOut, false);
    }

    if (optCell) {
        auto translatedCell =
            TranslatedCell{optCell->arrayInfo, *parent, optCell->tags, optCell->vals};

        addCellToObject(translatedCell, *outObj);
    }
}


PlanState ColumnScanStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    // We are about to call next() on a storage cursor so do not bother saving our internal state in
    // case it yields as the state will be completely overwritten after the next() call.
    disableSlotAccess();

    checkForInterrupt(_opCtx);

    if (_firstGetNext) {
        for (size_t i = 0; i < _columnCursors.size(); ++i) {
            _columnCursors[i].next();
        }
        _firstGetNext = false;
    }

    // Find minimum record ID of all column cursors.
    _recordId = RecordId();
    for (auto& cursor : _columnCursors) {
        auto& result = cursor.lastCell;
        if (result && (_recordId.isNull() || result->rid < _recordId)) {
            _recordId = result->rid;
        }
    }

    if (_recordId.isNull()) {
        return trackPlanState(PlanState::IS_EOF);
    }

    auto [outTag, outVal] = value::makeNewObject();
    auto& outObj = *value::bitcastTo<value::Object*>(outVal);
    value::ValueGuard materializedObjGuard(outTag, outVal);

    StringDataSet parentPathsRead;
    bool useRowStore = false;
    for (size_t i = 0; i < _columnCursors.size(); ++i) {
        auto& lastCell = _columnCursors[i].lastCell;

        const FakeCell* cellForRid =
            (lastCell && lastCell->rid == _recordId) ? lastCell.get_ptr() : nullptr;
        const auto& path = _columnCursors[i].cursor->path();

        if (cellForRid && (cellForRid->hasSubPaths || cellForRid->hasDuplicateFields)) {
            useRowStore = true;
        } else if (!useRowStore && _columnCursors[i].includeInOutput) {
            if (!cellForRid || cellForRid->isSparse) {
                // Must read in the parent information first.
                readParentsIntoObj(path, &outObj, &parentPathsRead);
            }

            if (cellForRid) {
                auto translatedCell = TranslatedCell{cellForRid->arrayInfo,
                                                     _columnCursors[i].path(),
                                                     cellForRid->tags,
                                                     cellForRid->vals};

                addCellToObject(translatedCell, outObj);
            }
        }

        if (cellForRid) {
            _columnCursors[i].next();
        }
    }

    if (useRowStore) {
        std::cout << "Using row store for rid " << _recordId << std::endl;

        // TODO: In some cases we can avoid calling seek() on the row store cursor, and instead do
        // a next() which should be much cheaper.
        auto record = _rowStoreCursor->seekExact(_recordId);

        // If there's no record, the index is out of sync with the row store.
        invariant(record);

        _rowStoreAccessor->reset(false,
                                 value::TypeTags::bsonObject,
                                 value::bitcastFrom<const char*>(record->data.data()));

        if (_recordExpr) {
            auto [owned, tag, val] = _bytecode.run(_recordExprCode.get());
            _recordAccessor->reset(owned, tag, val);
        }
    } else {
        _recordAccessor->reset(true, outTag, outVal);
        materializedObjGuard.reset();
    }

    if (_recordIdAccessor) {
        _recordIdAccessor->reset(
            false, value::TypeTags::RecordId, value::bitcastFrom<RecordId*>(&_recordId));
    }

    for (size_t idx = 0; idx < _outputFields.size(); ++idx) {
        auto [owned, tag, val] = _bytecode.run(_pathExprsCode[idx].get());
        _outputFields[idx].reset(owned, tag, val);
    }

    ++_specificStats.numReads;
    if (_tracker && _tracker->trackProgress<TrialRunTracker::kNumReads>(1)) {
        // If we're collecting execution stats during multi-planning and reached the end of the
        // trial period because we've performed enough physical reads, bail out from the trial run
        // by raising a special exception to signal a runtime planner that this candidate plan has
        // completed its trial run early. Note that a trial period is executed only once per a
        // PlanStage tree, and once completed never run again on the same tree.
        _tracker = nullptr;
        uasserted(ErrorCodes::QueryTrialRunCompleted, "Trial run early exit in scan");
    }
    return trackPlanState(PlanState::ADVANCED);
}

void ColumnScanStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _rowStoreCursor.reset();
    _coll.reset();
    _columnCursors.clear();
    _parentPathCursors.clear();
    _open = false;
}

std::unique_ptr<PlanStageStats> ColumnScanStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<ScanStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.append("columnIndexName", _columnIndexName);
        bob.appendNumber("numReads", static_cast<long long>(_specificStats.numReads));

        bob.append("paths", _paths);
        bob.append("outputSlots", _fieldSlots.begin(), _fieldSlots.end());

        ret->debugInfo = bob.obj();
    }
    return ret;
}

const SpecificStats* ColumnScanStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> ColumnScanStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    // Print out output slots.
    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _fieldSlots.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _fieldSlots[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    if (_recordSlot) {
        DebugPrinter::addIdentifier(ret, _recordSlot.get());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_recordIdSlot) {
        DebugPrinter::addIdentifier(ret, _recordIdSlot.get());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    // Print out paths.
    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _paths.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        ret.emplace_back(str::stream() << "\"" << _paths[idx] << "\"");
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back("@\"`");
    DebugPrinter::addIdentifier(ret, _collUuid.toString());
    ret.emplace_back("`\"");

    ret.emplace_back("@\"`");
    DebugPrinter::addIdentifier(ret, _columnIndexName);
    ret.emplace_back("`\"");

    return ret;
}

size_t ColumnScanStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_fieldSlots);
    size += size_estimator::estimate(_paths);
    size += size_estimator::estimate(_specificStats);
    return size;
}
}  // namespace sbe
}  // namespace mongo
