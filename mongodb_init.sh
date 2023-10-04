#!/bin/sh
export MONGO_HOST="localhost"
export MONGO_PORT="27017"
export MONGO_USER_NAME="edb"
export MONGO_PWD="edb"

# Below commands must be run in MongoDB to create mongo_fdw_regress and mongo_fdw_regress1 databases
# used in regression tests with edb user and edb password.

# use mongo_fdw_regress
# db.createUser({user:"edb",pwd:"edb",roles:[{role:"dbOwner", db:"mongo_fdw_regress"},{role:"readWrite", db:"mongo_fdw_regress"}]})
# use mongo_fdw_regress1
# db.createUser({user:"edb",pwd:"edb",roles:[{role:"dbOwner", db:"mongo_fdw_regress1"},{role:"readWrite", db:"mongo_fdw_regress1"}]})
# use mongo_fdw_regress2
# db.createUser({user:"edb",pwd:"edb",roles:[{role:"dbOwner", db:"mongo_fdw_regress2"},{role:"readWrite", db:"mongo_fdw_regress2"}]})

mongoimport --host=$MONGO_HOST --port=$MONGO_PORT -u $MONGO_USER_NAME -p $MONGO_PWD --db mongo_fdw_regress --collection countries --jsonArray --drop --maintainInsertionOrder --quiet < data/mongo_fixture.json
mongoimport --host=$MONGO_HOST --port=$MONGO_PORT -u $MONGO_USER_NAME -p $MONGO_PWD --db mongo_fdw_regress --collection warehouse --jsonArray --drop --maintainInsertionOrder --quiet < data/mongo_warehouse.json
mongo --host=$MONGO_HOST --port=$MONGO_PORT -u $MONGO_USER_NAME -p $MONGO_PWD --authenticationDatabase "mongo_fdw_regress" < data/mongo_test_data.js > /dev/null
