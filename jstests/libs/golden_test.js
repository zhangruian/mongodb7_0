
function tojsonOnelineSortKeys(x) {
    let indent = " ";
    let nolint = true;
    let depth = undefined;
    let sortKeys = true;
    return tojson(x, indent, nolint, depth, sortKeys);
}

// Takes an array of documents.
// - Discards the field ordering, by recursively sorting the fields of each object.
// - Discards the result-set ordering by sorting the array of normalized documents.
// Returns a string.
function normalize(result) {
    return result.map(d => tojsonOnelineSortKeys(d)).sort().join('\n') + '\n';
}

// Override print to output to both stdout and the golden file.
// This affects everything that uses print: printjson, jsTestLog, etc.
print = (() => {
    const original = print;
    return function print(...args) {
        // Imitate GlobalInfo::Functions::print::call.
        const str = args.map(a => a == null ? '[unknown type]' : a).join(' ');
        _writeGoldenData(str);

        return original(...args);
    };
})();

// Takes an array or cursor, and prints a normalized version of it.
//
// Normalizing means ignoring:
// - order of fields in a document
// - order of documents in the array/cursor.
//
// If running the query fails, this catches and prints the exception.
function show(cursorOrArray) {
    if (!Array.isArray(cursorOrArray)) {
        try {
            cursorOrArray = cursorOrArray.toArray();
        } catch (e) {
            print(tojson(e));
            return;
        }
    }

    print(normalize(cursorOrArray));
}

// Run any set-up necessary for a golden jstest.
// This function should be called from the suite definition, so that individual tests don't need
// to remember to call it. This function should not be called from any libs/*.js file, because
// it's surprising if load() has side effects (besides defining JS functions / values).
function beginGoldenTest() {
    _openGoldenData(jsTestName());
}
