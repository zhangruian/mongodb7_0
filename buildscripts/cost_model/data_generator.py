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
"""Implements to populate MongoDB collections with generated data used to calibrate Cost Model."""

from __future__ import annotations
from dataclasses import dataclass
from importlib.metadata import distribution
import time
import random
from typing import Sequence
import pymongo
from pymongo import InsertOne, IndexModel
from pymongo.collection import Collection
from random_generator import RandomDistribution
from common import timer_decorator
from config import DataGeneratorConfig, DataType
from database_instance import DatabaseInstance
from random_generator_config import distributions

__all__ = ['DataGenerator']


@dataclass
class FieldInfo:
    """Field-related information."""

    name: str
    type: DataType
    distribution: RandomDistribution


@dataclass
class CollectionInfo:
    """Collection-related information."""

    name: str
    fields: Sequence[FieldInfo]
    documents_count: int


class DataGenerator:
    """Create and populate collections with generated data."""

    def __init__(self, database: DatabaseInstance, config: DataGeneratorConfig):
        """Create new DataGenerator.

        Keyword Arguments:
        database -- Instance of Database object
        stringlength -- Length of generated strings
        """

        self.database = database
        self.config = config

        self.collection_infos = list(self._generate_collection_infos())

    def populate_collections(self) -> None:
        """Create and populate collections for each combination of size and data type in the corresponding 'docCounts' and 'dataTypes' input arrays.

        All collections have the same schema defined by one of the elements of 'collFields'.
        """

        if not self.config.enabled:
            return

        self.database.enable_cascades(False)
        t0 = time.time()
        for coll_info in self.collection_infos:
            coll = self.database.database.get_collection(coll_info.name)
            coll.drop()
            self._populate_collection(coll, coll_info)
            create_single_field_indexes(coll, coll_info.fields)
            create_compound_index(coll, coll_info.fields)

        t1 = time.time()
        print(f'\npopulate Collections took {t1-t0} s.')

    def _generate_collection_infos(self):
        for coll_template in self.config.collection_templates:
            fields = [
                FieldInfo(name=ft.name, type=ft.data_type,
                          distribution=distributions[ft.distribution])
                for ft in coll_template.fields
            ]
            for doc_count in self.config.collection_cardinalities:
                name = f'{coll_template.name}_{doc_count}'
                yield CollectionInfo(name=name, fields=fields, documents_count=doc_count)

    @timer_decorator
    def _populate_collection(self, coll: Collection, coll_info: CollectionInfo) -> None:
        print(f'\nGenerating ${coll_info.name} ...')
        batch_size = self.config.batch_size
        for _ in range(coll_info.documents_count // batch_size):
            populate_batch(coll, batch_size, coll_info.fields)
        if coll_info.documents_count % batch_size > 0:
            populate_batch(coll, coll_info.documents_count % batch_size, coll_info.fields)


def populate_batch(coll: Collection, documents_count: int, fields: Sequence[FieldInfo]) -> None:
    """Generate collection data and write it to the collection."""

    requests = [InsertOne(doc) for doc in generate_collection_data(documents_count, fields)]
    coll.bulk_write(requests, ordered=False)


def generate_collection_data(documents_count: int, fields: Sequence[FieldInfo]):
    """Generate random data for the specified fields of a collection."""

    documents = [{} for _ in range(documents_count)]
    for field in fields:
        for field_index, field_data in enumerate(field.distribution.generate(documents_count)):
            documents[field_index][field.name] = field_data
    return documents


def create_single_field_indexes(coll: Collection, fields: Sequence[FieldInfo]) -> None:
    """Create single-fields indexes on the given collection."""

    t0 = time.time()

    indexes = [IndexModel([(field.name, pymongo.ASCENDING)]) for field in fields]
    coll.create_indexes(indexes)

    t1 = time.time()
    print(f'createSingleFieldIndexes took {t1 - t0} s.')


def create_compound_index(coll: Collection, fields: Sequence[FieldInfo]) -> None:
    """Create a coumpound index on the given collection."""

    field_names = [fi.name for fi in fields if fi.type != DataType.ARRAY]
    if len(field_names) < 2:
        print(f'Collection: {coll.name} not suitable for compound index')
        return

    t0 = time.time()

    index_spec = [(field, pymongo.ASCENDING) for field in field_names]
    coll.create_index(index_spec)

    t1 = time.time()
    print(f'createCompoundIndex took {t1 - t0} s.')
