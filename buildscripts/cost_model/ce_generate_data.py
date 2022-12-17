# Copyright (C) 2022-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.
#
"""Data generation entry point."""

import asyncio
import dataclasses
import json
import os
import subprocess
from pathlib import Path
from bson.json_util import dumps
from config import CollectionTemplate, FieldTemplate, DataType
from data_generator import CollectionInfo, DataGenerator
from database_instance import DatabaseInstance
import parameters_extractor
from ce_generate_data_settings import database_config, data_generator_config

__all__ = []


class CollectionTemplateEncoder(json.JSONEncoder):
    def default(self, o):
        if isinstance(o, CollectionTemplate):
            collections = []
            for card in o.cardinalities:
                name = f'{o.name}_{card}'
                collections.append(
                    dict(collectionName=name, fields=o.fields, compound_indexes=o.compound_indexes,
                         cardinality=card))
            return collections
        elif isinstance(o, FieldTemplate):
            return dict(fieldName=o.name, data_type=o.data_type, indexed=o.indexed)
        elif isinstance(o, DataType):
            return o.name.lower()
        # Let the base class default method raise the TypeError
        return super(CollectionTemplateEncoder, self).default(o)


class OidEncoder(json.JSONEncoder):
    def default(self, o):
        # TODO: doesn't work, what is the type of OectIds?
        #if isinstance(o, OectId):
        if hasattr(o, '__str__'):  # This will handle OectIds
            return str(o)
        return super(OidEncoder, self).default(o)


async def dump_collection_to_json(db, dump_path, database_name, collections):
    with open(Path(dump_path) / f'{database_name}.data', "w") as data_file:
        data_file.write('// This is a generated file.\n')
        data_file.write('const dataSet = [\n')
        coll_pos = 1
        for coll_name in collections:
            collection = db[coll_name]
            doc_count = await collection.count_documents({})
            doc_pos = 1
            data_file.write(f'{{collName: "{coll_name}", collData: [\n')
            async for doc in collection.find({}):
                #data_file.write(dumps(doc))
                data_file.write(json.dumps(doc, cls=OidEncoder))
                if doc_pos < doc_count:
                    data_file.write(',')
                data_file.write("\n")
                doc_pos += 1
            data_file.write(']}')
            if coll_pos < len(collections):
                data_file.write(",")
        data_file.write("]\n")


async def main():
    """Entry point function."""
    script_directory = os.path.abspath(os.path.dirname(__file__))
    os.chdir(script_directory)

    # 1. Database Instance provides connectivity to a MongoDB instance, it loads data optionally
    # from the dump on creating and stores data optionally to the dump on closing.
    with DatabaseInstance(database_config) as database_instance:

        # 2. Generate random data and populate collections with it.
        generator = DataGenerator(database_instance, data_generator_config)
        await generator.populate_collections()

        # 3. Export all collections in the database into json files.
        db_collections = await database_instance.database.list_collection_names()
        #for coll_name in db_collections:
        # subprocess.run([
        #     'mongoexport', f'--db={database_config.database_name}', f'--collection={coll_name}',
        #     f'--out={coll_name}.dat'
        # ], cwd=database_config.dump_path, check=True)
        await dump_collection_to_json(database_instance.database, database_config.dump_path,
                                      database_config.database_name, db_collections)

        # 4. Export the collection templates used to create the test collections into JSON file
        with open(Path(database_config.dump_path) / f'{database_config.database_name}.schema',
                  "w") as metadata_file:
            collections = []
            for coll_template in data_generator_config.collection_templates:
                for card in coll_template.cardinalities:
                    name = f'{coll_template.name}_{card}'
                    collections.append(
                        dict(collectionName=name, fields=coll_template.fields,
                             compound_indexes=coll_template.compound_indexes, cardinality=card))
            json_metadata = json.dumps(collections, indent=4, cls=CollectionTemplateEncoder)
            metadata_file.write("// This is a generated file.\nconst dbMetadata = ")
            metadata_file.write(json_metadata)
            metadata_file.write(";")

    print("DONE!")


if __name__ == '__main__':
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    asyncio.run(main())
