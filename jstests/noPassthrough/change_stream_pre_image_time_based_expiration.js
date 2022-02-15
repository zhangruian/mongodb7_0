// Tests time-based pre-image retention policy of change stream pre-images remover job.
// @tags: [
//  requires_fcv_53,
//  featureFlagChangeStreamPreAndPostImages,
//  featureFlagChangeStreamPreAndPostImagesTimeBasedRetentionPolicy,
// ]
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");  // For configureFailPoint.

// Helper to verify if expected pre-images are present in pre-image collection.
function verifyPreImages(preImageColl, expectedPreImages, collectionsInfo) {
    const preImageDocs = preImageColl.find().toArray();

    assert.eq(preImageDocs.length, expectedPreImages.length, preImageDocs);

    for (let idx = 0; idx < preImageDocs.length; idx++) {
        const [collIdx, preImageId] = expectedPreImages[idx];
        const nsUUID = collectionsInfo[collIdx]["info"].uuid;

        assert.eq(preImageDocs[idx]._id.nsUUID,
                  nsUUID,
                  "pre-image in collection: " + tojson(preImageDocs[idx]) +
                      ", expected collIdx: " + collIdx + ", docIdx: " + idx);

        assert.eq(preImageDocs[idx].preImage._id,
                  preImageId,
                  "pre-image in collection: " + tojson(preImageDocs[idx]) +
                      ", expected _id: " + preImageId);
    }
}

// Tests time-based change stream pre-image retention policy.
function testTimeBasedPreImageRetentionPolicy(conn, primary) {
    // Annotations for pre-images that define if pre-image is expected to expire or not.
    const shouldExpire = "shouldExpire";
    const shouldRetain = "shouldRetain";

    // Each element defines a sequence of documents belonging to one collection.
    // Each documents has has associated status - 'expire' or 'retain'. This structure models the
    // state of the pre-images associated with a particular document of the collection, ie. the
    // pre-images corresponding to the documents 'shouldExpire' will be deleted by the remover job
    // and those with 'shouldRetain' will be retained.
    const docsStatePerCollection = [
        [shouldRetain],
        [shouldExpire],
        [shouldRetain, shouldExpire],
        [shouldExpire, shouldRetain],
        [shouldRetain, shouldRetain],
        [shouldExpire, shouldExpire]
    ];

    const collectionCount = docsStatePerCollection.length;
    const testDB = conn.getDB("test");

    // Create several collections with pre- and post-images enabled.
    for (let collIdx = 0; collIdx < collectionCount; collIdx++) {
        const collName = "coll" + collIdx;
        assert.commandWorked(
            testDB.createCollection(collName, {changeStreamPreAndPostImages: {enabled: true}}));
    }

    // Get the collections information and sort them by uuid. The pre-image documents are naturally
    // sorted first by uuid and then by timestamp in a pre-images collection. Sorting of collection
    // helps in setting up of the pre-images in the required order. The 'collectionsInfo' has
    // one-to-one mapping with 'docsStatePerCollection'.
    let collectionsInfo = testDB.getCollectionInfos();
    assert.eq(collectionsInfo.length, collectionCount);
    collectionsInfo.sort((coll1, coll2) => {
        return coll1.info.uuid <= coll2.info.uuid ? -1 : 1;
    });

    // Disable pre-image time-based expiration policy.
    assert.commandWorked(conn.getDB("admin").runCommand(
        {setChangeStreamOptions: 1, preAndPostImages: {expireAfterSeconds: "off"}}));

    let shouldRetainDocs = [];
    let shouldExpireDocs = [];
    let allDocs = [];

    // Insert documents to each collection. Iterate through the documents and group them as
    // 'shouldExpire' or 'shouldRetain' based on the document states. Each element of these groups
    // is an array, where the first element is the collection index and the second is the document
    // index.
    for (let collIdx = 0; collIdx < collectionCount; collIdx++) {
        const collName = collectionsInfo[collIdx]["name"];
        const coll = testDB.getCollection(collName);
        const docs = docsStatePerCollection[collIdx];

        for (let docIdx = 0; docIdx < docs.length; docIdx++) {
            assert.commandWorked(coll.insert({_id: docIdx}, {$set: {documentState: "inserted"}}));

            const documentState = docs[docIdx];
            allDocs.push([collIdx, docIdx]);
            if (documentState !== shouldExpire) {
                shouldRetainDocs.push([collIdx, docIdx]);
            } else {
                shouldExpireDocs.push([collIdx, docIdx]);
            }
        }
    }

    // The test case will first update the documents with documentState as 'shouldExpire' and then
    // with 'shouldRetain'. To correctly infer the ordering of the pre-images in the collection, the
    // sorting has to be done in such a way that for a each document, the documentState
    // 'shouldExpire' should come before the documentState 'shouldRetain'.
    allDocs.sort((doc1, doc2) => {
        const [collIdx1, docIdx1] = doc1;
        const [collIdx2, docIdx2] = doc2;

        const annotation1 = docsStatePerCollection[collIdx1][docIdx1];
        const annotation2 = docsStatePerCollection[collIdx2][docIdx2];

        // If documents are from different collections or if they are from the same collection but
        // have same document states, the preserve the original ordering.
        if (collIdx1 != collIdx2 || annotation1 == annotation2) {
            return 0;
        }

        // If documents belong to the same collection and have different document states, then
        // document with 'shouldExpire' should come first.
        return annotation1 == shouldExpire ? -1 : 1;
    });

    // Helper to update the document in the collection.
    const updateDocument = (docInfo) => {
        const [collIdx, docIdx] = docInfo;
        const collName = collectionsInfo[collIdx]["name"];
        const coll = testDB.getCollection(collName);

        assert.commandWorked(coll.updateOne({_id: docIdx}, {$set: {documentState: "updated"}}));
    };

    // Update each document that should expire, this will create pre-images.
    shouldExpireDocs.forEach(updateDocument);

    const preImageColl = primary.getDB("config").getCollection("system.preimages");
    const expireAfterSeconds = 1;

    // Verify that pre-images to be expired is recorded.
    verifyPreImages(preImageColl, shouldExpireDocs, collectionsInfo);

    // Get the last pre-image that should expire and compute the current time using that. The
    // current time is computed by adding (expireAfterSeconds + 0.001) seconds to the operation time
    // of the last recorded pre-image.
    const lastPreImageToExpire = preImageColl.find().sort({"_id.ts": -1}).limit(1).toArray();
    assert.eq(lastPreImageToExpire.length, 1, lastPreImageToExpire);
    const preImageShouldExpireAfter =
        lastPreImageToExpire[0].operationTime.getTime() + expireAfterSeconds * 1000;
    const currentTime = new Date(preImageShouldExpireAfter + 1);

    // Sleep for 1 ms before doing the next updates. The will ensure that the difference in
    // operation time between pre-images to be retained and pre-images to be expired is at least
    // 1 ms.
    sleep(1);

    // Update each document for which pre-images will be retain. These pre-images will be followed
    // by pre-images that should expire for a particular collection.
    shouldRetainDocs.forEach(updateDocument);

    // Verify that all pre-images are recorded.
    verifyPreImages(preImageColl, allDocs, collectionsInfo);

    // Configure the current time for the pre-image remover job. At this point, the time-based
    // pre-image expiration is still disabled.
    const currentTimeFailPoint =
        configureFailPoint(primary,
                           "changeStreamPreImageRemoverCurrentTime",
                           {currentTimeForTimeBasedExpiration: currentTime});

    // Wait until at least 1 complete cycle of pre-image removal job is completed.
    currentTimeFailPoint.wait(kDefaultWaitForFailPointTimeout, 2);

    // Verify that when time-based pre-image expiration disabled, no pre-images are not deleted.
    verifyPreImages(preImageColl, allDocs, collectionsInfo);

    // Enable time-based pre-image expiration and configure the 'expireAfterSeconds' to 1 seconds.
    assert.commandWorked(conn.getDB("admin").runCommand(
        {setChangeStreamOptions: 1, preAndPostImages: {expireAfterSeconds: expireAfterSeconds}}));

    // Verify that at some point in time, all expired pre-images will be deleted.
    assert.soon(() => {
        return preImageColl.find().toArray().length == shouldRetainDocs.length;
    });

    // Verify that pre-images corresponding to documents with document states 'shouldRetain' are
    // present.
    verifyPreImages(preImageColl, shouldRetainDocs, collectionsInfo);

    currentTimeFailPoint.off();
}

// Tests pre-image time based expiration on a replica-set.
// TODO SERVER-61802: Add test cases for shared cluster.
(function testChangeStreamPreImagesforTimeBasedExpirationOnReplicaSet() {
    const replSetTest = new ReplSetTest({name: "replSet", nodes: 1});
    replSetTest.startSet();
    replSetTest.initiate();

    const conn = replSetTest.getPrimary();
    const primary = replSetTest.getPrimary();
    testTimeBasedPreImageRetentionPolicy(conn, primary);
    replSetTest.stopSet();
})();
}());
