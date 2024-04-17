// Provides function to get the number of local reads via logs.
load("jstests/libs/log.js");

function logLinesExceededBufferSize(node) {
    const log = assert.commandWorked(node.adminCommand({getLog: "global"}));
    return (log.totalLinesWritten > 1024);
}

function enableLocalReadLogs(node) {
    assert.commandWorked(
        node.adminCommand({setParameter: 1, logComponentVerbosity: {query: {verbosity: 3}}}));
}

function getLocalReadCount(node, namespace, comment) {
    if (logLinesExceededBufferSize(node)) {
        jsTestLog('Warning: total log lines written since start of test is more than internal ' +
                  'buffer size. Some local read log lines may be missing!');
    }
    const log = assert.commandWorked(node.adminCommand({getLog: "global"})).log;
    return [
        ...findMatchingLogLines(log, {id: 5837600, namespace, comment: {comment: comment}})
    ].length;
}
