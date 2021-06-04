(function() {
"use strict";

const coll = db[jsTestName()];
coll.drop();

const nDocs = 10;
let currentDate = new Date();
for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(coll.insert({
        a: i,
        date: currentDate,
        partition: i % 2,
        partitionSeq: Math.trunc(i / 2),
    }));

    const nextDate = currentDate.getDate() + 1;
    currentDate.setDate(nextDate);
}

// The list of window functions to test.
const functions = {
    sum: {$sum: '$a'},
    avg: {$avg: '$a'},
    stdDevSamp: {$stdDevSamp: '$a'},
    stdDevPop: {$stdDevPop: '$a'},
    min: {$min: '$a'},
    max: {$max: '$a'},
    count: {$count: {}},
    derivative: {$derivative: {input: '$a'}},
    derivative_date: {$derivative: {input: '$date', outputUnit: 'millisecond'}},
    integral: {$integral: {input: '$a'}},
    integral_date: {$integral: {input: '$a', outputUnit: 'millisecond'}},
    covariancePop: {$covariancePop: ['$a', '$date']},
    covarianceSamp: {$covarianceSamp: ['$a', '$date']},
    expMovingAvgN: {$expMovingAvg: {input: '$a', N: 3}},
    expMovingAvgAlpha: {$expMovingAvg: {input: '$a', alpha: 0.1}},
    push: {$push: '$a'},
    addToSet: {$addToSet: '$a'},
    first: {$first: '$a'},
    last: {$last: '$a'},
    shift: {$shift: {output: '$a', by: 1, default: 0}},
    documentNumber: {$documentNumber: {}},
    rank: {$rank: {}},
    denseRank: {$denseRank: {}}
};

// The list of window definitions to test.
const windows = {
    none: null,
    left_unbounded_doc: {documents: ['unbounded', 'current']},
    right_unbounded_doc: {documents: ['current', 'unbounded']},
    past_doc: {documents: [-1, 'current']},
    future_doc: {documents: ['current', 1]},
    centered_doc: {documents: [-1, 1]},
    full_unbounded_range: {range: ['unbounded', 'unbounded']},
    left_unbounded_range: {range: ['unbounded', 'current']},
    right_unbounded_range: {range: ['current', 'unbounded']},
    past_range: {range: [-2, 'current']},
    future_range: {range: ['current', 2]},
    centered_range: {range: [-2, 2]},
};

// The list of sort definitions to test.
const sortBys = {
    none: null,
    asc: {partitionSeq: 1},
    desc: {partitionSeq: -1},
    asc_date: {date: 1},
    desc_date: {date: -1},
    multi: {partitionSeq: 1, partition: 1},
};

// The list of partition definitions to test.
const partitionBys = {
    none: null,
    field: '$partition'
};

// Given an element from each of the lists above, construct a
// $setWindowFields stage.
function constructQuery(wf, window, sortBy, partitionBy) {
    let pathArg = Object.assign({}, wf);
    if (window != null) {
        Object.assign(pathArg, {window: window});
    }

    let setWindowFieldsArg = {};

    if (sortBy != null) {
        setWindowFieldsArg.sortBy = sortBy;
    }

    if (partitionBy != null) {
        setWindowFieldsArg.partitionBy = partitionBy;
    }

    setWindowFieldsArg.output = {x: pathArg};

    return {$setWindowFields: setWindowFieldsArg};
}

// Given an element of each of the lists above, what is the expected
// result.  The output should be 'SKIP', 'OK' or the expected integer
// error code.
function expectedResult(wfType, windowType, sortType, partitionType) {
    // Skip range windows over dates or that are over descending windows.
    if (windowType.endsWith('range')) {
        if (sortType.endsWith('date') || sortType.startsWith('desc')) {
            return 'SKIP';
        }
    }

    // Derivative and integral require an ascending sort
    // and an explicit window.
    if (wfType.startsWith('derivative')) {
        // Derivative requires a sort and an explicit window.
        if (sortType == 'none' || windowType == 'none') {
            return ErrorCodes.FailedToParse;
        }

        // Integral requires single column sort
        if (sortType == 'multi') {
            return ErrorCodes.FailedToParse;
        }

        if (wfType == 'derivative_date' && !sortType.endsWith('date')) {
            // "$derivative with outputUnit expects the sortBy field to be a Date".
            return 5624900;
        }

        if (sortType.endsWith('date') && wfType != 'derivative_date') {
            // "$derivative where the sortBy is a Date requires an 'outputUnit'.
            return 5624901;
        }
    } else if (wfType.startsWith('integral')) {
        // Integral requires a sort.
        if (sortType == 'none') {
            return ErrorCodes.FailedToParse;
        }

        // Integral requires single column sort
        if (sortType == 'multi') {
            return ErrorCodes.FailedToParse;
        }

        if (wfType == 'integral_date' && !sortType.endsWith('date')) {
            // "$integral with outputUnit expects the sortBy field to be a Date"
            return 5423901;
        }

        if (sortType.endsWith('date') && wfType != 'integral_date') {
            // $integral where the sortBy is a Date requires an 'outputUnit'
            return 5423902;
        }
    } else if (wfType.startsWith('expMovingAvg')) {
        // $expMovingAvg doesn't accept a window.
        if (windowType != 'none') {
            return ErrorCodes.FailedToParse;
        }

        // $expMovingAvg requires a sort.
        if (sortType == 'none') {
            return ErrorCodes.FailedToParse;
        }
    } else if (wfType == 'documentNumber' || wfType == 'rank' || wfType == 'denseRank') {
        if (windowType != 'none') {
            // Rank style window functions take no other arguments.
            return 5371601;
        }

        if (sortType == 'none') {
            // %s must be specified with a top level sortBy expression with exactly one element.
            return 5371602;
        }

        if (sortType == 'multi') {
            // %s must be specified with a top level sortBy expression with exactly one element.
            return 5371602;
        }
    } else if (wfType == 'shift') {
        // $shift requires a sortBy and can't have defined a window.
        if (sortType == 'none' || windowType != 'none') {
            return ErrorCodes.FailedToParse;
        }
    }

    // Document windows require a sortBy.
    if (windowType.endsWith('doc') && sortType == 'none') {
        // 'Document-based bounds require a sortBy'.
        return 5339901;
    }

    // Range based windows require a sort over a single field.
    if (windowType.endsWith('range') && (sortType == 'none' || sortType == 'multi')) {
        // 'Range-based window require sortBy a single field'.
        return 5339902;
    }

    return ErrorCodes.OK;
}

// Generate all combinations of the elements in the lists above,
// one element per list.
function* makeTests() {
    for (const [wfType, wfDefinition] of Object.entries(functions)) {
        for (const [windowType, windowDefinition] of Object.entries(windows)) {
            for (const [sortType, sortDefinition] of Object.entries(sortBys)) {
                for (const [partitionType, partitionDefinition] of Object.entries(partitionBys)) {
                    let test = {
                        query: constructQuery(
                            wfDefinition, windowDefinition, sortDefinition, partitionDefinition),
                        expectedResult: expectedResult(wfType, windowType, sortType, partitionType)
                    };

                    if (test.expectedResult == 'SKIP') {
                        continue;
                    }

                    yield test;
                }
            }
        }
    }
}

// Run all the combinations generated in makeTests.
for (const test of makeTests()) {
    if (test.expectedResult == ErrorCodes.OK) {
        assert.commandWorked(
            coll.runCommand({aggregate: coll.getName(), pipeline: [test.query], cursor: {}}));
    } else {
        assert.commandFailedWithCode(
            coll.runCommand({aggregate: coll.getName(), pipeline: [test.query], cursor: {}}),
            test.expectedResult);
    }
}
})();
