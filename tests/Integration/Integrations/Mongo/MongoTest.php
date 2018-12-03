<?php

namespace DDTrace\Tests\Integration\Integrations\Mongo;

use MongoId;
use MongoClient;
use DDTrace\Integrations\Mongo\MongoIntegration;
use DDTrace\Tests\Integration\Common\SpanAssertion;
use DDTrace\Tests\Integration\Common\IntegrationTestCase;

final class MongoTest extends IntegrationTestCase
{
    const HOST = 'mongodb_integration';
    const PORT = '27017';
    const USER = 'test';
    const PASSWORD = 'test';
    const DATABASE = 'test';

    public static function setUpBeforeClass()
    {
        if (!extension_loaded('mongo')) {
            self::markTestSkipped('The mongo extension is required to run the MongoDB tests.');
        }
        parent::setUpBeforeClass();
        MongoIntegration::load();
    }

    protected function tearDown()
    {
        $this->clearDatabase();
        parent::tearDown();
    }

    public function testClientConnectAndClose()
    {
        $traces = $this->isolateTracer(function () {
            $mongo = self::getClient();
            $mongo->close(true);
        });

        $this->assertSpans($traces, [
            SpanAssertion::build('MongoClient.__construct', 'mongo', 'mongodb', '__construct')
                ->withExactTags([
                    'mongodb.server' => 'mongodb://mongodb_integration:27017',
                    'mongodb.db' => 'test',
                ]),
        ]);
    }

    public function testSecretsAreSanitizedFromDsnString()
    {
        $traces = $this->isolateTracer(function () {
            $mongo = new MongoClient(
                sprintf(
                    'mongodb://%s:%s@%s:%s',
                    self::USER,
                    self::PASSWORD,
                    self::HOST,
                    self::PORT
                ),
                [
                    'db' => self::DATABASE,
                ]
            );
            $mongo->close(true);
        });

        $this->assertSpans($traces, [
            SpanAssertion::build('MongoClient.__construct', 'mongo', 'mongodb', '__construct')
                ->withExactTags([
                    'mongodb.server' => 'mongodb://?:?@mongodb_integration:27017',
                    'mongodb.db' => 'test',
                ]),
        ]);
    }

    public function testDatabaseNameExtractedFromDsnString()
    {
        $traces = $this->isolateTracer(function () {
            $mongo = new MongoClient(
                sprintf(
                    'mongodb://%s:%s/%s',
                    self::HOST,
                    self::PORT,
                    self::DATABASE
                ),
                [
                    'username' => self::USER,
                    'password' => self::PASSWORD,
                ]
            );
            $mongo->close(true);
        });

        $this->assertSpans($traces, [
            SpanAssertion::build('MongoClient.__construct', 'mongo', 'mongodb', '__construct')
                ->withExactTags([
                    'mongodb.server' => 'mongodb://mongodb_integration:27017/test',
                    'mongodb.db' => 'test',
                ]),
        ]);
    }

    public function testCommandWithQueryAndTimeout()
    {
        $traces = $this->isolateClient(function (MongoClient $mongo) {
            $mongo->{self::DATABASE}->command([
                'distinct' => 'people',
                'key' => 'age',
                'query' => ['age' => ['$gte' => 18]]
            ], ['socketTimeoutMS' => 500]);
        });

        $this->assertSpans($traces, [
            SpanAssertion::build('MongoDB.command', 'mongo', 'mongodb', 'command')
                ->withExactTags([
                    'mongodb.query' => '{"age":{"$gte":18}}',
                    'mongodb.timeout' => '500',
                ]),
        ]);
    }

    public function testCreateDBRef()
    {
        $traces = $this->isolateClient(function (MongoClient $mongo) {
            $mongo->{self::DATABASE}->createDBRef('foo_collection', new MongoId('47cc67093475061e3d9536d2'));
        });

        $this->assertSpans($traces, [
            SpanAssertion::build('MongoDB.createDBRef', 'mongo', 'mongodb', 'createDBRef')
                ->withExactTags([
                    'mongodb.collection' => 'foo_collection',
                    'mongodb.bson.id' => '47cc67093475061e3d9536d2',
                ]),
        ]);
    }

    public function testCreateCollection()
    {
        $traces = $this->isolateClient(function (MongoClient $mongo) {
            $mongo->{self::DATABASE}->createCollection('foo_collection');
        });

        $this->assertSpans($traces, [
            SpanAssertion::build('MongoDB.createCollection', 'mongo', 'mongodb', 'createCollection')
                ->withExactTags([
                    'mongodb.collection' => 'foo_collection',
                ]),
        ]);
    }

    public function testExecute()
    {
        $traces = $this->isolateClient(function (MongoClient $mongo) {
            $mongo->{self::DATABASE}->execute('"foo";');
        });

        $this->assertSpans($traces, [
            SpanAssertion::build('MongoDB.execute', 'mongo', 'mongodb', 'execute'),
        ]);
    }

    public function testGetDBRef()
    {
        $traces = $this->isolateClient(function (MongoClient $mongo) {
            $mongo->{self::DATABASE}->getDBRef([
                '$ref' => 'foo_collection',
                '$id' => new MongoId('47cc67093475061e3d9536d2'),
            ]);
        });

        $this->assertSpans($traces, [
            SpanAssertion::build('MongoDB.getDBRef', 'mongo', 'mongodb', 'getDBRef')
                ->withExactTags([
                    'mongodb.collection' => 'foo_collection',
                ]),
        ]);
    }

    public function testSelectCollection()
    {
        $traces = $this->isolateClient(function (MongoClient $mongo) {
            $mongo->{self::DATABASE}->selectCollection('foo_collection');
        });

        $this->assertSpans($traces, [
            SpanAssertion::build('MongoDB.selectCollection', 'mongo', 'mongodb', 'selectCollection')
                ->withExactTags([
                    'mongodb.collection' => 'foo_collection',
                ]),
        ]);
    }

    public function testSetProfilingLevel()
    {
        $traces = $this->isolateClient(function (MongoClient $mongo) {
            $mongo->{self::DATABASE}->setProfilingLevel(2);
        });

        $this->assertSpans($traces, [
            SpanAssertion::build('MongoDB.setProfilingLevel', 'mongo', 'mongodb', 'setProfilingLevel')
                ->withExactTags([
                    'mongodb.profiling_level' => '2',
                ]),
        ]);
    }

    public function testSetReadPreference()
    {
        $traces = $this->isolateClient(function (MongoClient $mongo) {
            $mongo->{self::DATABASE}->setReadPreference(MongoClient::RP_NEAREST);
        });

        $this->assertSpans($traces, [
            SpanAssertion::build('MongoDB.setReadPreference', 'mongo', 'mongodb', 'setReadPreference')
                ->withExactTags([
                    'mongodb.read_preference' => MongoClient::RP_NEAREST,
                ]),
        ]);
    }

    public function testSetWriteConcern()
    {
        $traces = $this->isolateClient(function (MongoClient $mongo) {
            $mongo->{self::DATABASE}->setWriteConcern('foo');
        });

        $this->assertSpans($traces, [
            SpanAssertion::build('MongoDB.setWriteConcern', 'mongo', 'mongodb', 'setWriteConcern'),
        ]);
    }

    /**
     * @dataProvider dbMethods
     */
    public function testDBWithDefaultTags($method)
    {
        $traces = $this->isolateClient(function (MongoClient $mongo) use ($method) {
            $mongo->{self::DATABASE}->{$method}();
        });

        $this->assertSpans($traces, [
            SpanAssertion::build('MongoDB.' . $method, 'mongo', 'mongodb', $method),
        ]);
    }

    public function dbMethods()
    {
        return [
            ['drop'],
            ['forceError'],
            ['getCollectionInfo'],
            ['getCollectionNames'],
            ['getGridFS'],
            ['getProfilingLevel'],
            ['getReadPreference'],
            ['getSlaveOkay'],
            ['getWriteConcern'],
            ['lastError'],
            ['listCollections'],
            ['prevError'],
            ['repair'],
            ['resetError'],
            ['setSlaveOkay'],
        ];
    }

    private function isolateClient(\Closure $callback)
    {
        $mongo = self::getClient();
        $traces = $this->isolateTracer(function () use ($mongo, $callback) {
            $callback($mongo);
        });
        $mongo->close(true);
        return $traces;
    }

    private static function getClient()
    {
        return new MongoClient(
            'mongodb://' . self::HOST . ':' . self::PORT,
            [
                'username' => self::USER,
                'password' => self::PASSWORD,
                'db' => self::DATABASE,
            ]
        );
    }

    private function clearDatabase()
    {
        $this->isolateTracer(function () {
            $mongo = self::getClient();
            $mongo->{self::DATABASE}->drop();
            $mongo->close();
        });
    }
}
