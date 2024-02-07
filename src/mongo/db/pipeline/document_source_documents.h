/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/query/projection_parser.h"

namespace mongo {

namespace DocumentSourceDocuments {
class LiteParsed : public LiteParsedDocumentSource {
public:
    static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss, const BSONElement& spec) {
        return std::make_unique<LiteParsed>(spec.fieldName());
    }

    LiteParsed(std::string parseTimeName) : LiteParsedDocumentSource(std::move(parseTimeName)) {}

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
        return stdx::unordered_set<NamespaceString>();
    }

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final {
        return {};
    }

    bool isInitialSource() const final {
        return true;
    }

    bool isDocuments() const final {
        return true;
    }

    bool allowedToPassthroughFromMongos() const final {
        return false;
    }
};

static constexpr StringData kStageName = "$documents"_sd;

std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

};  // namespace DocumentSourceDocuments

}  // namespace mongo
