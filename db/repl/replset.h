// /db/repl/replset.h

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "../../util/concurrency/list.h"
#include "../../util/concurrency/value.h"
#include "../../util/hostandport.h"
#include "rstime.h"
#include "rsmember.h"
#include "rs_config.h"

namespace mongo {

    extern bool replSet; // true if using repl sets
    extern class ReplSet *theReplSet; // null until initialized

    extern Tee *rsLog;

    /* information about the entire repl set, such as the various servers in the set, and their state */
    /* note: We currently do not free mem when the set goes away - it is assumed the replset is a 
             singleton and long lived.
    */
    class ReplSet {
    public:
        static enum StartupStatus { 
            PRESTART=0, LOADINGCONFIG=1, BADCONFIG=2, EMPTYCONFIG=3, 
            EMPTYUNREACHABLE=4, STARTED=5, SOON=6 } startupStatus;
        static string startupStatusMsg;

    private: 
        MemberState _myState;

    public:
        void fatal();
        bool isMaster(const char *client);
        void fillIsMaster(BSONObjBuilder&);
        bool ok() const { return _myState != FATAL; }
        MemberState state() const { return _myState; }        
        string name() const { return _name; } /* @return replica set's logical name */

        /* cfgString format is 
           replsetname/host1,host2:port,...
           where :port is optional.

           throws exception if a problem initializing. */
        ReplSet(string cfgString);

        /* call after constructing to start - returns fairly quickly after launching its threads */
        void go() { _myState = STARTUP2; startHealthThreads(); }

        // for replSetGetStatus command
        void summarizeStatus(BSONObjBuilder&) const;
        void summarizeAsHtml(stringstream&) const;
        const ReplSetConfig& config() { return *_cfg; }

    private:
        string _name;
        const vector<HostAndPort> *_seeds;
        ReplSetConfig *_cfg;

        /** load our configuration from admin.replset.  try seed machines too. 
            throws exception if a problem.
        */
        void _loadConfigFinish(vector<ReplSetConfig>& v);
        void loadConfig();
        void initFromConfig(ReplSetConfig& c);//, bool save);

        class Consensus {
            ReplSet &rs;
            bool inprog;
            void _electSelf();
        public:
            Consensus(ReplSet *t) : rs(*t),inprog(false) { }
            int totalVotes() const;
            bool aMajoritySeemsToBeUp() const;
            void electSelf();
        } elect;

    public:
        struct Member : public List1<Member>::Base {
            Member(HostAndPort h, unsigned ord, const ReplSetConfig::MemberCfg *c);
            string fullName() const { return m().h().toString(); }
            const ReplSetConfig::MemberCfg& config() const { return *_config; }
            void summarizeAsHtml(stringstream& s) const;
            const RSMember& m() const { return _m; }
            string lhb() { return _m.lastHeartbeatMsg; }
        private:
            const ReplSetConfig::MemberCfg *_config; /* todo: when this changes??? */
            RSMember _m;
        };
        list<HostAndPort> memberHostnames() const;
        const Member* currentPrimary() const { return _currentPrimary; }
        const ReplSetConfig::MemberCfg& myConfig() const { return _self->config(); }

    private:
        const Member *_currentPrimary;

        static string stateAsStr(MemberState state);
        static string stateAsHtml(MemberState state);

        Member *_self;
        /* all members of the set EXCEPT self. */
        List1<Member> _members;
        Member* head() const { return _members.head(); }

        void startHealthThreads();
        friend class FeedbackThread;

    public:
        class Manager : boost::noncopyable {
            ReplSet *_rs;
            int _primary;
            const Member* findOtherPrimary();
            void noteARemoteIsPrimary(const Member *);
        public:
            Manager(ReplSet *rs);
            void checkNewState();
        } _mgr;

    };

    inline void ReplSet::fatal() 
    { _myState = FATAL; log() << "replSet error fatal error, stopping replication" << rsLog; }

    inline ReplSet::Member::Member(HostAndPort h, unsigned ord, const ReplSetConfig::MemberCfg *c) : 
        _config(c), _m(h, ord) { }

    inline bool ReplSet::isMaster(const char *client) {         
        /* todo replset */
        return false;
    }

}
