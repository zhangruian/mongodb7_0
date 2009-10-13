// security.cpp

/**
 *    Copyright (C) 2009 10gen Inc.
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

#include "stdafx.h"
#include "security.h"
#include "client.h"

namespace mongo {

    boost::thread_specific_ptr<Client> currentClient;

    Client::Client() : _ns(""), _nsstr("") { 
        ai = new AuthenticationInfo(); 
    }

    Client::~Client() { 
cout << "TEMP" << endl;
        delete ai; 
    }

    bool noauth = true;

	int AuthenticationInfo::warned;

} // namespace mongo

