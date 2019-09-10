// Copyright (C) MongoDB, Inc. 2019-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Code generated by operationgen. DO NOT EDIT.

package operation

import (
	"context"
	"errors"
	"fmt"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/event"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
	"go.mongodb.org/mongo-driver/x/mongo/driver"
	"go.mongodb.org/mongo-driver/x/mongo/driver/description"
	"go.mongodb.org/mongo-driver/x/mongo/driver/session"
)

// ListDatabases performs a listDatabases operation.
type ListDatabases struct {
	filter         bsoncore.Document
	nameOnly       *bool
	session        *session.Client
	clock          *session.ClusterClock
	monitor        *event.CommandMonitor
	database       string
	deployment     driver.Deployment
	readPreference *readpref.ReadPref
	retry          *driver.RetryMode
	selector       description.ServerSelector

	result ListDatabasesResult
}

type ListDatabasesResult struct {
	// An array of documents, one document for each database
	Databases []databaseRecord
	// The sum of the size of all the database files on disk in bytes.
	TotalSize int64
}

type databaseRecord struct {
	Name       string
	SizeOnDisk int64 `bson:"sizeOnDisk"`
	Empty      bool
}

func buildListDatabasesResult(response bsoncore.Document, srvr driver.Server) (ListDatabasesResult, error) {
	elements, err := response.Elements()
	if err != nil {
		return ListDatabasesResult{}, err
	}
	ir := ListDatabasesResult{}
	for _, element := range elements {
		switch element.Key() {

		case "totalSize":
			var ok bool
			ir.TotalSize, ok = element.Value().AsInt64OK()
			if !ok {
				err = fmt.Errorf("response field 'totalSize' is type int64, but received BSON type %s: %s", element.Value().Type, element.Value())
			}

		case "databases":
			// TODO: Make operationgen handle array results.
			arr, ok := element.Value().ArrayOK()
			if !ok {
				err = fmt.Errorf("response field 'databases' is type array, but received BSON type %s", element.Value().Type)
				continue
			}

			var tmp bsoncore.Document
			marshalErr := bson.Unmarshal(arr, &tmp)
			if marshalErr != nil {
				err = marshalErr
				continue
			}
			records, marshalErr := tmp.Elements()
			if marshalErr != nil {
				err = marshalErr
				continue
			}

			ir.Databases = make([]databaseRecord, len(records))
			for i, val := range records {
				valueDoc, ok := val.Value().DocumentOK()
				if !ok {
					err = fmt.Errorf("'databases' element is type document, but received BSON type %s", val.Value().Type)
					continue
				}

				elems, marshalErr := valueDoc.Elements()
				if marshalErr != nil {
					err = marshalErr
					continue
				}
				for _, elem := range elems {
					switch elem.Key() {

					case "name":
						ir.Databases[i].Name, ok = elem.Value().StringValueOK()
						if !ok {
							err = fmt.Errorf("response field 'name' is type string, but received BSON type %s", elem.Value().Type)
							continue
						}

					case "sizeOnDisk":
						ir.Databases[i].SizeOnDisk, ok = elem.Value().AsInt64OK()
						if !ok {
							err = fmt.Errorf("response field 'sizeOnDisk' is type int64, but received BSON type %s", elem.Value().Type)
							continue
						}

					case "empty":
						ir.Databases[i].Empty, ok = elem.Value().BooleanOK()
						if !ok {
							err = fmt.Errorf("response field 'empty' is type bool, but received BSON type %s", elem.Value().Type)
							continue
						}
					}
				}
			}
		}
	}
	return ir, err
}

// NewListDatabases constructs and returns a new ListDatabases.
func NewListDatabases(filter bsoncore.Document) *ListDatabases {
	return &ListDatabases{
		filter: filter,
	}
}

// Result returns the result of executing this operation.
func (ld *ListDatabases) Result() ListDatabasesResult { return ld.result }

func (ld *ListDatabases) processResponse(response bsoncore.Document, srvr driver.Server, desc description.Server) error {
	var err error

	ld.result, err = buildListDatabasesResult(response, srvr)
	return err

}

// Execute runs this operations and returns an error if the operaiton did not execute successfully.
func (ld *ListDatabases) Execute(ctx context.Context) error {
	if ld.deployment == nil {
		return errors.New("the ListDatabases operation must have a Deployment set before Execute can be called")
	}

	return driver.Operation{
		CommandFn:         ld.command,
		ProcessResponseFn: ld.processResponse,

		Client:         ld.session,
		Clock:          ld.clock,
		CommandMonitor: ld.monitor,
		Database:       ld.database,
		Deployment:     ld.deployment,
		ReadPreference: ld.readPreference,
		RetryMode:      ld.retry,
		Type:           driver.Read,
		Selector:       ld.selector,
	}.Execute(ctx, nil)

}

func (ld *ListDatabases) command(dst []byte, desc description.SelectedServer) ([]byte, error) {
	dst = bsoncore.AppendInt32Element(dst, "listDatabases", 1)
	if ld.filter != nil {

		dst = bsoncore.AppendDocumentElement(dst, "filter", ld.filter)
	}
	if ld.nameOnly != nil {

		dst = bsoncore.AppendBooleanElement(dst, "nameOnly", *ld.nameOnly)
	}

	return dst, nil
}

// Filter determines what results are returned from listDatabases.
func (ld *ListDatabases) Filter(filter bsoncore.Document) *ListDatabases {
	if ld == nil {
		ld = new(ListDatabases)
	}

	ld.filter = filter
	return ld
}

// NameOnly specifies whether to only return database names.
func (ld *ListDatabases) NameOnly(nameOnly bool) *ListDatabases {
	if ld == nil {
		ld = new(ListDatabases)
	}

	ld.nameOnly = &nameOnly
	return ld
}

// Session sets the session for this operation.
func (ld *ListDatabases) Session(session *session.Client) *ListDatabases {
	if ld == nil {
		ld = new(ListDatabases)
	}

	ld.session = session
	return ld
}

// ClusterClock sets the cluster clock for this operation.
func (ld *ListDatabases) ClusterClock(clock *session.ClusterClock) *ListDatabases {
	if ld == nil {
		ld = new(ListDatabases)
	}

	ld.clock = clock
	return ld
}

// CommandMonitor sets the monitor to use for APM events.
func (ld *ListDatabases) CommandMonitor(monitor *event.CommandMonitor) *ListDatabases {
	if ld == nil {
		ld = new(ListDatabases)
	}

	ld.monitor = monitor
	return ld
}

// Database sets the database to run this operation against.
func (ld *ListDatabases) Database(database string) *ListDatabases {
	if ld == nil {
		ld = new(ListDatabases)
	}

	ld.database = database
	return ld
}

// Deployment sets the deployment to use for this operation.
func (ld *ListDatabases) Deployment(deployment driver.Deployment) *ListDatabases {
	if ld == nil {
		ld = new(ListDatabases)
	}

	ld.deployment = deployment
	return ld
}

// ReadPreference set the read prefernce used with this operation.
func (ld *ListDatabases) ReadPreference(readPreference *readpref.ReadPref) *ListDatabases {
	if ld == nil {
		ld = new(ListDatabases)
	}

	ld.readPreference = readPreference
	return ld
}

// ServerSelector sets the selector used to retrieve a server.
func (ld *ListDatabases) ServerSelector(selector description.ServerSelector) *ListDatabases {
	if ld == nil {
		ld = new(ListDatabases)
	}

	ld.selector = selector
	return ld
}

// Retry enables retryable mode for this operation. Retries are handled automatically in driver.Operation.Execute based
// on how the operation is set.
func (ld *ListDatabases) Retry(retry driver.RetryMode) *ListDatabases {
	if ld == nil {
		ld = new(ListDatabases)
	}

	ld.retry = &retry
	return ld
}
