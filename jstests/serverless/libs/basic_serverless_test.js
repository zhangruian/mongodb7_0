class BasicServerlessTest {
    constructor({recipientTagName, recipientSetName}) {
        this.donor = new ReplSetTest({name: "donor", nodes: 3});
        this.donor.startSet();
        this.donor.initiate();

        this.recipientTagName = recipientTagName;
        this.recipientSetName = recipientSetName;
        this.recipientNodes = [];
    }

    stop() {
        // If we validate, it will try to list all collections and the migrated collections will
        // return a TenantMigrationCommitted error.
        this.donor.stopSet(undefined /* signal */, false /* forRestart */, {skipValidation: 1});
    }

    addRecipientNodes(numNodes) {
        numNodes = numNodes || 3;  // default to three nodes

        if (this.recipientNodes.lengh > 0) {
            throw new Error("Recipient nodes may only be added once");
        }

        jsTestLog(`Adding ${numNodes} non-voting recipient nodes to donor`);
        const donor = this.donor;
        for (let i = 0; i < numNodes; ++i) {
            this.recipientNodes.push(donor.add());
        }

        const primary = donor.getPrimary();
        const admin = primary.getDB('admin');
        const config = donor.getReplSetConfigFromNode();
        config.version++;

        // ensure recipient nodes are added as non-voting members
        this.recipientNodes.forEach(node => {
            config.members.push({
                host: node.host,
                votes: 0,
                priority: 0,
                tags: {[this.recipientTagName]: ObjectId().valueOf()}
            });
        });

        // reindex all members from 0
        config.members = config.members.map((member, idx) => {
            member._id = idx;
            return member;
        });

        assert.commandWorked(admin.runCommand({replSetReconfig: config}));
        this.recipientNodes.forEach(node => donor.waitForState(node, ReplSetTest.State.SECONDARY));
    }
}
