/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/query/sbe_stage_builder.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/stages/text_match.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/fts/fts_index_format.h"
#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/query/sbe_stage_builder_coll_scan.h"
#include "mongo/db/query/sbe_stage_builder_filter.h"
#include "mongo/db/query/sbe_stage_builder_index_scan.h"
#include "mongo/db/query/sbe_stage_builder_projection.h"
#include "mongo/db/query/util/make_data_structure.h"

namespace mongo::stage_builder {
std::unique_ptr<sbe::RuntimeEnvironment> makeRuntimeEnvironment(
    OperationContext* opCtx, sbe::value::SlotIdGenerator* slotIdGenerator) {
    auto env = std::make_unique<sbe::RuntimeEnvironment>();

    // Register an unowned global timezone database for datetime expression evaluation.
    env->registerSlot("timeZoneDB"_sd,
                      sbe::value::TypeTags::timeZoneDB,
                      sbe::value::bitcastFrom<const TimeZoneDatabase*>(getTimeZoneDatabase(opCtx)),
                      false,
                      slotIdGenerator);
    return env;
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::buildCollScan(
    const QuerySolutionNode* root) {
    auto csn = static_cast<const CollectionScanNode*>(root);
    auto [resultSlot, recordIdSlot, oplogTsSlot, stage] =
        generateCollScan(_opCtx,
                         _collection,
                         csn,
                         &_slotIdGenerator,
                         &_frameIdGenerator,
                         _yieldPolicy,
                         _data.env,
                         _isTailableCollScanResumeBranch,
                         _data.trialRunProgressTracker.get());
    _data.resultSlot = resultSlot;
    _data.recordIdSlot = recordIdSlot;
    _data.oplogTsSlot = oplogTsSlot;
    _data.shouldTrackLatestOplogTimestamp = csn->shouldTrackLatestOplogTimestamp;
    _data.shouldTrackResumeToken = csn->requestResumeToken;
    _data.shouldUseTailableScan = csn->tailable;

    if (_returnKeySlot) {
        // Assign the '_returnKeySlot' to be the empty object.
        stage = sbe::makeProjectStage(std::move(stage),
                                      root->nodeId(),
                                      *_returnKeySlot,
                                      sbe::makeE<sbe::EFunction>("newObj", sbe::makeEs()));
    }

    return std::move(stage);
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::buildIndexScan(
    const QuerySolutionNode* root) {
    auto ixn = static_cast<const IndexScanNode*>(root);
    auto [recordIdSlot, indexKeySlots, stage] =
        generateIndexScan(_opCtx,
                          _collection,
                          ixn,
                          _returnKeySlot,
                          _indexKeysToInclude.value_or(sbe::IndexKeysInclusionSet{}),
                          &_slotIdGenerator,
                          &_spoolIdGenerator,
                          _yieldPolicy,
                          _data.trialRunProgressTracker.get());

    _data.recordIdSlot = recordIdSlot;

    if (_indexKeysToInclude) {
        _indexKeySlots = std::move(indexKeySlots);
    }

    return std::move(stage);
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::makeLoopJoinForFetch(
    std::unique_ptr<sbe::PlanStage> inputStage,
    sbe::value::SlotId recordIdKeySlot,
    PlanNodeId planNodeId,
    const sbe::value::SlotVector& slotsToForward) {
    _data.resultSlot = _slotIdGenerator.generate();
    _data.recordIdSlot = _slotIdGenerator.generate();

    // Scan the collection in the range [recordIdKeySlot, Inf).
    auto scanStage = sbe::makeS<sbe::ScanStage>(
        NamespaceStringOrUUID{_collection->ns().db().toString(), _collection->uuid()},
        _data.resultSlot,
        _data.recordIdSlot,
        std::vector<std::string>{},
        sbe::makeSV(),
        recordIdKeySlot,
        true,
        nullptr,
        _data.trialRunProgressTracker.get(),
        planNodeId);

    // Get the recordIdKeySlot from the outer side (e.g., IXSCAN) and feed it to the inner side,
    // limiting the result set to 1 row.
    return sbe::makeS<sbe::LoopJoinStage>(
        std::move(inputStage),
        sbe::makeS<sbe::LimitSkipStage>(std::move(scanStage), 1, boost::none, planNodeId),
        std::move(slotsToForward),
        sbe::makeSV(recordIdKeySlot),
        nullptr,
        planNodeId);
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::buildFetch(const QuerySolutionNode* root) {
    auto fn = static_cast<const FetchNode*>(root);
    auto inputStage = build(fn->children[0]);

    uassert(4822880, "RecordId slot is not defined", _data.recordIdSlot);

    auto stage =
        makeLoopJoinForFetch(std::move(inputStage),
                             *_data.recordIdSlot,
                             root->nodeId(),
                             _returnKeySlot ? sbe::makeSV(*_returnKeySlot) : sbe::makeSV());

    if (fn->filter) {
        auto relevantSlots = sbe::makeSV(*_data.resultSlot, *_data.recordIdSlot);
        if (_returnKeySlot) {
            relevantSlots.push_back(*_returnKeySlot);
        }

        stage = generateFilter(_opCtx,
                               fn->filter.get(),
                               std::move(stage),
                               &_slotIdGenerator,
                               &_frameIdGenerator,
                               *_data.resultSlot,
                               _data.env,
                               std::move(relevantSlots),
                               root->nodeId());
    }

    return stage;
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::buildLimit(const QuerySolutionNode* root) {
    const auto ln = static_cast<const LimitNode*>(root);
    // If we have both limit and skip stages and the skip stage is beneath the limit, then we
    // can combine these two stages into one. So, save the _limit value and let the skip stage
    // builder handle it.
    if (ln->children[0]->getType() == StageType::STAGE_SKIP) {
        _limit = ln->limit;
    }

    auto inputStage = build(ln->children[0]);
    return _limit || _isTailableCollScanResumeBranch
        ? std::move(inputStage)
        : std::make_unique<sbe::LimitSkipStage>(
              std::move(inputStage), ln->limit, boost::none, root->nodeId());
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::buildSkip(const QuerySolutionNode* root) {
    const auto sn = static_cast<const SkipNode*>(root);
    auto inputStage = build(sn->children[0]);
    return _isTailableCollScanResumeBranch
        ? std::move(inputStage)
        : std::make_unique<sbe::LimitSkipStage>(
              std::move(inputStage), _limit, sn->skip, root->nodeId());
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::buildSort(const QuerySolutionNode* root) {
    // TODO SERVER-48470: Replace std::string_view with StringData.
    using namespace std::literals;

    const auto sn = static_cast<const SortNode*>(root);
    auto sortPattern = SortPattern{sn->pattern, _cq.getExpCtx()};
    auto inputStage = build(sn->children[0]);
    sbe::value::SlotVector orderBy;
    std::vector<sbe::value::SortDirection> direction;
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projectMap;

    for (const auto& part : sortPattern) {
        uassert(4822881, "Sorting by expression not supported", !part.expression);
        uassert(4822882,
                "Sorting by dotted paths not supported",
                part.fieldPath && part.fieldPath->getPathLength() == 1);

        // Slot holding the sort key.
        auto sortFieldVar{_slotIdGenerator.generate()};
        orderBy.push_back(sortFieldVar);
        direction.push_back(part.isAscending ? sbe::value::SortDirection::Ascending
                                             : sbe::value::SortDirection::Descending);

        // Generate projection to get the value of the sort key. Ideally, this should be
        // tracked by a 'reference tracker' at higher level.
        auto fieldName = part.fieldPath->getFieldName(0);
        auto fieldNameSV = std::string_view{fieldName.rawData(), fieldName.size()};
        projectMap.emplace(
            sortFieldVar,
            sbe::makeE<sbe::EFunction>("getField"sv,
                                       sbe::makeEs(sbe::makeE<sbe::EVariable>(*_data.resultSlot),
                                                   sbe::makeE<sbe::EConstant>(fieldNameSV))));
    }

    inputStage =
        sbe::makeS<sbe::ProjectStage>(std::move(inputStage), std::move(projectMap), root->nodeId());

    // Generate traversals to pick the min/max element from arrays.
    for (size_t idx = 0; idx < orderBy.size(); ++idx) {
        auto resultVar{_slotIdGenerator.generate()};
        auto innerVar{_slotIdGenerator.generate()};

        auto innerBranch = sbe::makeProjectStage(
            sbe::makeS<sbe::LimitSkipStage>(
                sbe::makeS<sbe::CoScanStage>(root->nodeId()), 1, boost::none, root->nodeId()),
            root->nodeId(),
            innerVar,
            sbe::makeE<sbe::EVariable>(orderBy[idx]));

        auto op = direction[idx] == sbe::value::SortDirection::Ascending
            ? sbe::EPrimBinary::less
            : sbe::EPrimBinary::greater;
        auto minmax = sbe::makeE<sbe::EIf>(
            sbe::makeE<sbe::EPrimBinary>(
                op,
                sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::cmp3w,
                                             sbe::makeE<sbe::EVariable>(innerVar),
                                             sbe::makeE<sbe::EVariable>(resultVar)),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                           sbe::value::bitcastFrom<int64_t>(0))),
            sbe::makeE<sbe::EVariable>(innerVar),
            sbe::makeE<sbe::EVariable>(resultVar));

        inputStage = sbe::makeS<sbe::TraverseStage>(std::move(inputStage),
                                                    std::move(innerBranch),
                                                    orderBy[idx],
                                                    resultVar,
                                                    innerVar,
                                                    sbe::makeSV(),
                                                    std::move(minmax),
                                                    nullptr,
                                                    root->nodeId(),
                                                    boost::none);
        orderBy[idx] = resultVar;
    }

    sbe::value::SlotVector values;
    values.push_back(*_data.resultSlot);
    if (_data.recordIdSlot) {
        // Break ties with record id if available.
        orderBy.push_back(*_data.recordIdSlot);
        // This is arbitrary.
        direction.push_back(sbe::value::SortDirection::Ascending);
    }

    // A sort stage is a binding reflector, so we need to plumb through the 'oplogTsSlot' to make
    // it visible at the root stage.
    if (_data.oplogTsSlot) {
        values.push_back(*_data.oplogTsSlot);
    }

    // The '_returnKeySlot' likewise needs to be visible at the root stage.
    if (_returnKeySlot) {
        values.push_back(*_returnKeySlot);
    }

    return sbe::makeS<sbe::SortStage>(std::move(inputStage),
                                      std::move(orderBy),
                                      std::move(direction),
                                      std::move(values),
                                      sn->limit ? sn->limit
                                                : std::numeric_limits<std::size_t>::max(),
                                      internalQueryMaxBlockingSortMemoryUsageBytes.load(),
                                      _cq.getExpCtx()->allowDiskUse,
                                      _data.trialRunProgressTracker.get(),
                                      root->nodeId());
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::buildSortKeyGeneraror(
    const QuerySolutionNode* root) {
    uasserted(4822883, "Sort key generator in not supported in SBE yet");
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::buildProjectionSimple(
    const QuerySolutionNode* root) {
    using namespace std::literals;

    auto pn = static_cast<const ProjectionNodeSimple*>(root);
    auto inputStage = build(pn->children[0]);
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projections;
    sbe::value::SlotVector fieldSlots;

    invariant(_data.resultSlot);

    for (const auto& field : pn->proj.getRequiredFields()) {
        fieldSlots.push_back(_slotIdGenerator.generate());
        projections.emplace(
            fieldSlots.back(),
            sbe::makeE<sbe::EFunction>("getField"sv,
                                       sbe::makeEs(sbe::makeE<sbe::EVariable>(*_data.resultSlot),
                                                   sbe::makeE<sbe::EConstant>(std::string_view{
                                                       field.c_str(), field.size()}))));
    }

    return sbe::makeS<sbe::MakeObjStage>(sbe::makeS<sbe::ProjectStage>(std::move(inputStage),
                                                                       std::move(projections),
                                                                       root->nodeId()),
                                         *_data.resultSlot,
                                         boost::none,
                                         std::vector<std::string>{},
                                         pn->proj.getRequiredFields(),
                                         fieldSlots,
                                         true,
                                         false,
                                         root->nodeId());
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::buildProjectionCovered(
    const QuerySolutionNode* root) {
    using namespace std::literals;

    auto pn = static_cast<const ProjectionNodeCovered*>(root);
    invariant(pn->proj.isSimple());

    // For now, we only support ProjectionNodeCovered when its child is an IndexScanNode.
    uassert(5037301,
            str::stream() << "Can't build exec tree for node: " << root->toString(),
            pn->children[0]->getType() == STAGE_IXSCAN);

    // If we're pulling data out of one index we can pre-compute the indices of the fields
    // in the key that we pull data from and avoid looking up the field name each time.
    invariant(!_indexKeysToInclude);
    _indexKeysToInclude.emplace();
    ON_BLOCK_EXIT([&] { _indexKeysToInclude.reset(); });

    std::vector<std::string> keyFieldNames;
    StringSet requiredFields = {pn->proj.getRequiredFields().begin(),
                                pn->proj.getRequiredFields().end()};

    // pn->coveredKeyObj is the "index.keyPattern" from the IndexScanNode or DistinctNode. This
    // lists all the fields that the index can provide, not the fields that the projection wants.
    // requiredFields lists all of the fields that the projection wants. Since this is a covered
    // projection, we're guaranteed that pn->coveredKeyObj contains all of the fields that the
    // projection wants.
    size_t i = 0;
    for (auto&& elt : pn->coveredKeyObj) {
        if (requiredFields.count(elt.fieldNameStringData())) {
            _indexKeysToInclude->set(i);
            keyFieldNames.push_back(elt.fieldName());
        }

        ++i;
    }

    auto inputStage = build(pn->children[0]);

    // Assert that the index scan produced values for this covered projection
    invariant(_indexKeySlots);
    ON_BLOCK_EXIT([&] { _indexKeySlots.reset(); });

    _data.resultSlot = _slotIdGenerator.generate();

    return sbe::makeS<sbe::MakeObjStage>(std::move(inputStage),
                                         *_data.resultSlot,
                                         boost::none,
                                         std::vector<std::string>{},
                                         std::move(keyFieldNames),
                                         std::move(*_indexKeySlots),
                                         true,
                                         false,
                                         root->nodeId());
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::buildProjectionDefault(
    const QuerySolutionNode* root) {
    using namespace std::literals;

    auto pn = static_cast<const ProjectionNodeDefault*>(root);
    auto inputStage = build(pn->children[0]);
    invariant(_data.resultSlot);
    auto [slot, stage] = generateProjection(_opCtx,
                                            &pn->proj,
                                            std::move(inputStage),
                                            &_slotIdGenerator,
                                            &_frameIdGenerator,
                                            *_data.resultSlot,
                                            _data.env,
                                            root->nodeId());
    _data.resultSlot = slot;
    return std::move(stage);
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::buildOr(const QuerySolutionNode* root) {
    std::vector<std::unique_ptr<sbe::PlanStage>> inputStages;
    std::vector<sbe::value::SlotVector> inputSlots;

    auto orn = static_cast<const OrNode*>(root);

    // Translate each child of the 'Or' node. Each child may produce new 'resultSlot' and
    // recordIdSlot' stored in the _data member. We need to add these slots into the 'inputSlots'
    // vector which is used as input to the union statge below.
    for (auto&& child : orn->children) {
        inputStages.push_back(build(child));
        invariant(_data.resultSlot);
        invariant(_data.recordIdSlot);
        inputSlots.push_back({*_data.resultSlot, *_data.recordIdSlot});
    }

    // Construct a union stage whose branches are translated children of the 'Or' node.
    _data.resultSlot = _slotIdGenerator.generate();
    _data.recordIdSlot = _slotIdGenerator.generate();
    auto stage = sbe::makeS<sbe::UnionStage>(std::move(inputStages),
                                             std::move(inputSlots),
                                             sbe::makeSV(*_data.resultSlot, *_data.recordIdSlot),
                                             root->nodeId());

    if (orn->dedup) {
        stage = sbe::makeS<sbe::HashAggStage>(
            std::move(stage), sbe::makeSV(*_data.recordIdSlot), sbe::makeEM(), root->nodeId());
    }

    if (orn->filter) {
        auto relevantSlots = sbe::makeSV(*_data.resultSlot, *_data.recordIdSlot);
        stage = generateFilter(_opCtx,
                               orn->filter.get(),
                               std::move(stage),
                               &_slotIdGenerator,
                               &_frameIdGenerator,
                               *_data.resultSlot,
                               _data.env,
                               std::move(relevantSlots),
                               root->nodeId());
    }

    return stage;
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::buildText(const QuerySolutionNode* root) {
    auto textNode = static_cast<const TextNode*>(root);

    invariant(_collection);
    auto&& indexName = textNode->index.identifier.catalogName;
    const auto desc = _collection->getIndexCatalog()->findIndexByName(_opCtx, indexName);
    invariant(desc);
    const auto accessMethod = static_cast<const FTSAccessMethod*>(
        _collection->getIndexCatalog()->getEntry(desc)->accessMethod());
    invariant(accessMethod);
    auto&& ftsSpec = accessMethod->getSpec();

    // We assume here that node->ftsQuery is an FTSQueryImpl, not an FTSQueryNoop. In practice, this
    // means that it is illegal to use the StageBuilder on a QuerySolution created by planning a
    // query that contains "no-op" expressions.
    auto ftsQuery = static_cast<fts::FTSQueryImpl&>(*textNode->ftsQuery);

    // A vector of the output slots for each index scan stage. Each stage outputs a record id and a
    // record, so we expect each inner vector to be of length two.
    std::vector<sbe::value::SlotVector> ixscanOutputSlots;

    const bool forward = true;
    const bool inclusive = true;
    auto makeKeyString = [&](const BSONObj& bsonKey) {
        return std::make_unique<KeyString::Value>(
            IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
                bsonKey,
                accessMethod->getSortedDataInterface()->getKeyStringVersion(),
                accessMethod->getSortedDataInterface()->getOrdering(),
                forward,
                inclusive));
    };

    std::vector<std::unique_ptr<sbe::PlanStage>> indexScanList;
    for (const auto& term : ftsQuery.getTermsForBounds()) {
        // TODO: Should we scan in the opposite direction?
        auto startKeyBson = fts::FTSIndexFormat::getIndexKey(
            0, term, textNode->indexPrefix, ftsSpec.getTextIndexVersion());
        auto endKeyBson = fts::FTSIndexFormat::getIndexKey(
            fts::MAX_WEIGHT, term, textNode->indexPrefix, ftsSpec.getTextIndexVersion());

        auto recordSlot = _slotIdGenerator.generate();
        auto&& [recordIdSlot, ixscan] =
            generateSingleIntervalIndexScan(_collection,
                                            indexName,
                                            forward,
                                            makeKeyString(startKeyBson),
                                            makeKeyString(endKeyBson),
                                            sbe::IndexKeysInclusionSet{},
                                            sbe::makeSV(),
                                            recordSlot,
                                            &_slotIdGenerator,
                                            _yieldPolicy,
                                            _data.trialRunProgressTracker.get(),
                                            root->nodeId());
        indexScanList.push_back(std::move(ixscan));
        ixscanOutputSlots.push_back(sbe::makeSV(recordIdSlot, recordSlot));
    }

    // Union will output a slot for the record id and another for the record.
    _data.recordIdSlot = _slotIdGenerator.generate();
    auto unionRecordOutputSlot = _slotIdGenerator.generate();
    auto unionOutputSlots = sbe::makeSV(*_data.recordIdSlot, unionRecordOutputSlot);

    // Index scan output slots become the input slots to the union.
    auto unionStage = sbe::makeS<sbe::UnionStage>(
        std::move(indexScanList), ixscanOutputSlots, unionOutputSlots, root->nodeId());

    // TODO: If text score metadata is requested, then we should sum over the text scores inside the
    // index keys for a given document. This will require expression evaluation to be able to
    // extract the score directly from the key string.
    auto hashAggStage = sbe::makeS<sbe::HashAggStage>(
        std::move(unionStage), sbe::makeSV(*_data.recordIdSlot), sbe::makeEM(), root->nodeId());

    auto nljStage =
        makeLoopJoinForFetch(std::move(hashAggStage), *_data.recordIdSlot, root->nodeId());

    // Add a special stage to apply 'ftsQuery' to matching documents, and then add a FilterStage to
    // discard documents which do not match.
    auto textMatchResultSlot = _slotIdGenerator.generate();
    auto textMatchStage = sbe::makeS<sbe::TextMatchStage>(std::move(nljStage),
                                                          ftsQuery,
                                                          ftsSpec,
                                                          *_data.resultSlot,
                                                          textMatchResultSlot,
                                                          root->nodeId());

    // Filter based on the contents of the slot filled out by the TextMatchStage.
    auto filteredStage = sbe::makeS<sbe::FilterStage<false>>(
        std::move(textMatchStage), sbe::makeE<sbe::EVariable>(textMatchResultSlot), root->nodeId());

    if (_returnKeySlot) {
        // Assign the '_returnKeySlot' to be the empty object.
        return sbe::makeProjectStage(std::move(filteredStage),
                                     root->nodeId(),
                                     *_returnKeySlot,
                                     sbe::makeE<sbe::EFunction>("newObj", sbe::makeEs()));
    } else {
        return filteredStage;
    }
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::buildReturnKey(
    const QuerySolutionNode* root) {
    // TODO SERVER-49509: If the projection includes {$meta: "sortKey"}, the result of this stage
    // should also include the sort key. Everything else in the projection is ignored.
    auto returnKeyNode = static_cast<const ReturnKeyNode*>(root);

    auto resultSlot = _slotIdGenerator.generate();
    invariant(!_data.resultSlot);
    _data.resultSlot = resultSlot;

    invariant(!_returnKeySlot);
    _returnKeySlot = _slotIdGenerator.generate();

    auto stage = build(returnKeyNode->children[0]);
    _data.resultSlot = *_returnKeySlot;
    return stage;
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::makeUnionForTailableCollScan(
    const QuerySolutionNode* root) {
    using namespace std::literals;

    // Register a SlotId in the global environment which would contain a recordId to resume a
    // tailable collection scan from. A PlanStage executor will track the last seen recordId and
    // will reset a SlotAccessor for the resumeRecordIdSlot with this recordId.
    auto resumeRecordIdSlot = _data.env->registerSlot(
        "resumeRecordId"_sd, sbe::value::TypeTags::Nothing, 0, false, &_slotIdGenerator);

    // For tailable collection scan we need to build a special union sub-tree consisting of two
    // branches:
    //   1) An anchor branch implementing an initial collection scan before the first EOF is hit.
    //   2) A resume branch implementing all consecutive collection scans from a recordId which was
    //      seen last.
    //
    // The 'makeStage' parameter is used to build a PlanStage tree which is served as a root stage
    // for each of the union branches. The same machanism is used to build each union branch, and
    // the special logic which needs to be triggered depending on which branch we build is
    // controlled by setting the _isTailableCollScanResumeBranch flag.

    _isBuildingUnionForTailableCollScan = true;

    auto makeUnionBranch = [&](bool isTailableCollScanResumeBranch)
        -> std::pair<sbe::value::SlotVector, std::unique_ptr<sbe::PlanStage>> {
        _isTailableCollScanResumeBranch = isTailableCollScanResumeBranch;
        auto branch = build(root);
        auto branchSlots = sbe::makeSV(*_data.resultSlot, *_data.recordIdSlot);
        if (_data.oplogTsSlot) {
            branchSlots.push_back(*_data.oplogTsSlot);
        }
        if (_returnKeySlot) {
            branchSlots.push_back(*_returnKeySlot);
        }
        return {std::move(branchSlots), std::move(branch)};
    };

    // Build an anchor branch of the union and add a constant filter on top of it, so that it would
    // only execute on an initial collection scan, that is, when resumeRecordId is not available
    // yet.
    auto&& [anchorBranchSlots, anchorBranch] = makeUnionBranch(false);
    anchorBranch = sbe::makeS<sbe::FilterStage<true>>(
        std::move(anchorBranch),
        sbe::makeE<sbe::EPrimUnary>(
            sbe::EPrimUnary::logicNot,
            sbe::makeE<sbe::EFunction>(
                "exists"sv, sbe::makeEs(sbe::makeE<sbe::EVariable>(resumeRecordIdSlot)))),
        root->nodeId());

    // Build a resume branch of the union and add a constant filter on op of it, so that it would
    // only execute when we resume a collection scan from the resumeRecordId.
    auto&& [resumeBranchSlots, resumeBranch] = makeUnionBranch(true);
    resumeBranch = sbe::makeS<sbe::FilterStage<true>>(
        sbe::makeS<sbe::LimitSkipStage>(std::move(resumeBranch), boost::none, 1, root->nodeId()),
        sbe::makeE<sbe::EFunction>("exists"sv,
                                   sbe::makeEs(sbe::makeE<sbe::EVariable>(resumeRecordIdSlot))),
        root->nodeId());

    invariant(anchorBranchSlots.size() == resumeBranchSlots.size());

    // A vector of the output slots for each union branch.
    auto branchSlots = makeVector<sbe::value::SlotVector>(std::move(anchorBranchSlots),
                                                          std::move(resumeBranchSlots));

    _data.resultSlot = _slotIdGenerator.generate();
    _data.recordIdSlot = _slotIdGenerator.generate();
    auto unionOutputSlots = sbe::makeSV(*_data.resultSlot, *_data.recordIdSlot);
    if (_data.oplogTsSlot) {
        _data.oplogTsSlot = _slotIdGenerator.generate();
        unionOutputSlots.push_back(*_data.oplogTsSlot);
    }

    // Branch output slots become the input slots to the union.
    auto unionStage =
        sbe::makeS<sbe::UnionStage>(makeVector<std::unique_ptr<sbe::PlanStage>>(
                                        std::move(anchorBranch), std::move(resumeBranch)),
                                    branchSlots,
                                    unionOutputSlots,
                                    root->nodeId());
    _isBuildingUnionForTailableCollScan = false;
    return unionStage;
}

// Returns a non-null pointer to the root of a plan tree, or a non-OK status if the PlanStage tree
// could not be constructed.
std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::build(const QuerySolutionNode* root) {
    static const stdx::unordered_map<StageType,
                                     std::function<std::unique_ptr<sbe::PlanStage>(
                                         SlotBasedStageBuilder&, const QuerySolutionNode* root)>>
        kStageBuilders = {
            {STAGE_COLLSCAN, std::mem_fn(&SlotBasedStageBuilder::buildCollScan)},
            {STAGE_IXSCAN, std::mem_fn(&SlotBasedStageBuilder::buildIndexScan)},
            {STAGE_FETCH, std::mem_fn(&SlotBasedStageBuilder::buildFetch)},
            {STAGE_LIMIT, std::mem_fn(&SlotBasedStageBuilder::buildLimit)},
            {STAGE_SKIP, std::mem_fn(&SlotBasedStageBuilder::buildSkip)},
            {STAGE_SORT_SIMPLE, std::mem_fn(&SlotBasedStageBuilder::buildSort)},
            {STAGE_SORT_DEFAULT, std::mem_fn(&SlotBasedStageBuilder::buildSort)},
            {STAGE_SORT_KEY_GENERATOR, std::mem_fn(&SlotBasedStageBuilder::buildSortKeyGeneraror)},
            {STAGE_PROJECTION_SIMPLE, std::mem_fn(&SlotBasedStageBuilder::buildProjectionSimple)},
            {STAGE_PROJECTION_DEFAULT, std::mem_fn(&SlotBasedStageBuilder::buildProjectionDefault)},
            {STAGE_PROJECTION_COVERED, std::mem_fn(&SlotBasedStageBuilder::buildProjectionCovered)},
            {STAGE_OR, &SlotBasedStageBuilder::buildOr},
            {STAGE_TEXT, &SlotBasedStageBuilder::buildText},
            {STAGE_RETURN_KEY, &SlotBasedStageBuilder::buildReturnKey}};

    uassert(4822884,
            str::stream() << "Can't build exec tree for node: " << root->toString(),
            kStageBuilders.find(root->getType()) != kStageBuilders.end());

    // If this plan is for a tailable cursor scan, and we're not already in the process of building
    // a special union sub-tree implementing such scans, then start building a union sub-tree. Note
    // that LIMIT or SKIP stage is used as a splitting point of the two union branches, if present,
    // because we need to apply limit (or skip) only in the initial scan (in the anchor branch), and
    // the resume branch should not have it.
    switch (root->getType()) {
        case STAGE_COLLSCAN:
        case STAGE_LIMIT:
        case STAGE_SKIP:
            if (_cq.getQueryRequest().isTailable() && !_isBuildingUnionForTailableCollScan) {
                return makeUnionForTailableCollScan(root);
            }
        default:
            break;
    }

    return std::invoke(kStageBuilders.at(root->getType()), *this, root);
}
}  // namespace mongo::stage_builder
