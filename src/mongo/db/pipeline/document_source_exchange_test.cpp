/**
 *    Copyright (C) 2018 MongoDB Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_exchange.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/time_support.h"


namespace mongo {

class DocumentSourceExchangeTest : public AggregationContextFixture {
protected:
    std::unique_ptr<executor::TaskExecutor> _executor;
    virtual void setUp() override {
        auto net = executor::makeNetworkInterface("ExchangeTest");

        ThreadPool::Options options;
        auto pool = std::make_unique<ThreadPool>(options);

        _executor =
            std::make_unique<executor::ThreadPoolTaskExecutor>(std::move(pool), std::move(net));
        _executor->startup();
    }

    virtual void tearDown() override {
        _executor->shutdown();
        _executor.reset();
    }

    auto getMockSource(int cnt) {
        auto source = DocumentSourceMock::create();
        for (int i = 0; i < cnt; ++i)
            source->queue.emplace_back(Document{{"a", i}, {"b", "aaaaaaaaaaaaaaaaaaaaaaaaaaa"_sd}});

        return source;
    }

    static auto getNewSeed() {
        auto seed = Date_t::now().asInt64();
        unittest::log() << "Generated new seed is " << seed;

        return seed;
    }

    auto getRandomMockSource(size_t cnt, int64_t seed) {
        PseudoRandom prng(seed);

        auto source = DocumentSourceMock::create();
        for (size_t i = 0; i < cnt; ++i)
            source->queue.emplace_back(Document{{"a", static_cast<int>(prng.nextInt32() % cnt)},
                                                {"b", "aaaaaaaaaaaaaaaaaaaaaaaaaaa"_sd}});

        return source;
    }
};

TEST_F(DocumentSourceExchangeTest, SimpleExchange1Consumer) {
    const size_t nDocs = 500;

    auto source = getMockSource(nDocs);

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kRoundRobin);
    spec.setConsumers(1);
    spec.setBufferSize(1024);

    boost::intrusive_ptr<Exchange> ex = new Exchange(spec);

    ex->setSource(source.get());

    auto input = ex->getNext(0);

    size_t docs = 0;
    for (; input.isAdvanced(); input = ex->getNext(0)) {
        ++docs;
    }

    ASSERT_EQ(docs, nDocs);
}

TEST_F(DocumentSourceExchangeTest, SimpleExchangeNConsumer) {
    const size_t nDocs = 500;
    auto source = getMockSource(500);

    const size_t nConsumers = 5;

    ASSERT_EQ(nDocs % nConsumers, 0u);

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kRoundRobin);
    spec.setConsumers(nConsumers);
    spec.setBufferSize(1024);

    boost::intrusive_ptr<Exchange> ex = new Exchange(spec);

    std::vector<boost::intrusive_ptr<DocumentSourceExchange>> prods;

    for (size_t idx = 0; idx < nConsumers; ++idx) {
        prods.push_back(new DocumentSourceExchange(getExpCtx(), ex, idx));
        prods.back()->setSource(source.get());
    }

    std::vector<executor::TaskExecutor::CallbackHandle> handles;

    for (size_t id = 0; id < nConsumers; ++id) {
        auto handle = _executor->scheduleWork(
            [prods, id, nDocs, nConsumers](const executor::TaskExecutor::CallbackArgs& cb) {
                PseudoRandom prng(getNewSeed());

                auto input = prods[id]->getNext();

                size_t docs = 0;

                for (; input.isAdvanced(); input = prods[id]->getNext()) {
                    sleepmillis(prng.nextInt32() % 20 + 1);
                    ++docs;
                }
                ASSERT_EQ(docs, nDocs / nConsumers);
            });

        handles.emplace_back(std::move(handle.getValue()));
    }

    for (auto& h : handles)
        _executor->wait(h);
}

TEST_F(DocumentSourceExchangeTest, BroadcastExchangeNConsumer) {
    const size_t nDocs = 500;
    auto source = getMockSource(nDocs);

    const size_t nConsumers = 5;

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kBroadcast);
    spec.setConsumers(nConsumers);
    spec.setBufferSize(1024);

    boost::intrusive_ptr<Exchange> ex = new Exchange(spec);

    std::vector<boost::intrusive_ptr<DocumentSourceExchange>> prods;

    for (size_t idx = 0; idx < nConsumers; ++idx) {
        prods.push_back(new DocumentSourceExchange(getExpCtx(), ex, idx));
        prods.back()->setSource(source.get());
    }

    std::vector<executor::TaskExecutor::CallbackHandle> handles;

    for (size_t id = 0; id < nConsumers; ++id) {
        auto handle = _executor->scheduleWork(
            [prods, id, nDocs](const executor::TaskExecutor::CallbackArgs& cb) {
                size_t docs = 0;
                for (auto input = prods[id]->getNext(); input.isAdvanced();
                     input = prods[id]->getNext()) {
                    ++docs;
                }
                ASSERT_EQ(docs, nDocs);
            });

        handles.emplace_back(std::move(handle.getValue()));
    }

    for (auto& h : handles)
        _executor->wait(h);
}

TEST_F(DocumentSourceExchangeTest, RangeExchangeNConsumer) {
    const size_t nDocs = 500;
    auto source = getMockSource(nDocs);

    std::vector<BSONObj> boundaries;
    boundaries.push_back(BSON("a" << MINKEY));
    boundaries.push_back(BSON("a" << 100));
    boundaries.push_back(BSON("a" << 200));
    boundaries.push_back(BSON("a" << 300));
    boundaries.push_back(BSON("a" << 400));
    boundaries.push_back(BSON("a" << MAXKEY));

    const size_t nConsumers = boundaries.size() - 1;

    ASSERT(nDocs % nConsumers == 0);

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kRange);
    spec.setKey(BSON("a" << 1));
    spec.setBoundaries(boundaries);
    spec.setConsumers(nConsumers);
    spec.setBufferSize(1024);

    boost::intrusive_ptr<Exchange> ex = new Exchange(spec);

    std::vector<boost::intrusive_ptr<DocumentSourceExchange>> prods;

    for (size_t idx = 0; idx < nConsumers; ++idx) {
        prods.push_back(new DocumentSourceExchange(getExpCtx(), ex, idx));
        prods.back()->setSource(source.get());
    }

    std::vector<executor::TaskExecutor::CallbackHandle> handles;

    for (size_t id = 0; id < nConsumers; ++id) {
        auto handle = _executor->scheduleWork(
            [prods, id, nDocs, nConsumers](const executor::TaskExecutor::CallbackArgs& cb) {
                size_t docs = 0;
                for (auto input = prods[id]->getNext(); input.isAdvanced();
                     input = prods[id]->getNext()) {
                    size_t value = input.getDocument()["a"].getInt();

                    ASSERT(value >= id * 100);
                    ASSERT(value < (id + 1) * 100);

                    ++docs;
                }

                ASSERT_EQ(docs, nDocs / nConsumers);
            });

        handles.emplace_back(std::move(handle.getValue()));
    }

    for (auto& h : handles)
        _executor->wait(h);
}

TEST_F(DocumentSourceExchangeTest, RangeRandomExchangeNConsumer) {
    const size_t nDocs = 500;
    auto source = getRandomMockSource(nDocs, getNewSeed());

    std::vector<BSONObj> boundaries;
    boundaries.push_back(BSON("a" << MINKEY));
    boundaries.push_back(BSON("a" << 100));
    boundaries.push_back(BSON("a" << 200));
    boundaries.push_back(BSON("a" << 300));
    boundaries.push_back(BSON("a" << 400));
    boundaries.push_back(BSON("a" << MAXKEY));

    const size_t nConsumers = boundaries.size() - 1;

    ASSERT(nDocs % nConsumers == 0);

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kRange);
    spec.setKey(BSON("a" << 1));
    spec.setBoundaries(boundaries);
    spec.setConsumers(nConsumers);
    spec.setBufferSize(1024);

    boost::intrusive_ptr<Exchange> ex = new Exchange(spec);

    std::vector<boost::intrusive_ptr<DocumentSourceExchange>> prods;

    for (size_t idx = 0; idx < nConsumers; ++idx) {
        prods.push_back(new DocumentSourceExchange(getExpCtx(), ex, idx));
        prods.back()->setSource(source.get());
    }

    std::vector<executor::TaskExecutor::CallbackHandle> handles;

    AtomicWord<size_t> processedDocs{0};

    for (size_t id = 0; id < nConsumers; ++id) {
        auto handle = _executor->scheduleWork(
            [prods, id, &processedDocs](const executor::TaskExecutor::CallbackArgs& cb) {
                PseudoRandom prng(getNewSeed());

                auto input = prods[id]->getNext();

                size_t docs = 0;
                for (; input.isAdvanced(); input = prods[id]->getNext()) {
                    size_t value = input.getDocument()["a"].getInt();

                    ASSERT(value >= id * 100);
                    ASSERT(value < (id + 1) * 100);

                    ++docs;

                    // This helps randomizing thread scheduling forcing different threads to load
                    // buffers. The sleep API is inherently imprecise so we cannot guarantee 100%
                    // reproducibility.
                    sleepmillis(prng.nextInt32() % 50 + 1);
                }
                processedDocs.fetchAndAdd(docs);
            });

        handles.emplace_back(std::move(handle.getValue()));
    }

    for (auto& h : handles)
        _executor->wait(h);

    ASSERT_EQ(nDocs, processedDocs.load());
}
}  // namespace mongo
