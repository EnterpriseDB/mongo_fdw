#!/bin/sh


# create database and collection
printf "use pgtest\ndb.createCollection('pgtesttable')" | mongo


# create user
printf "use pgtest\ndb.createUser({ user: 'test', pwd: 'connecttest', roles: [ 'readWrite' ] })" | mongo


# insert data into collection
printf "use pgtest\ndb.pgtesttable.insert( { question: '42', answer: 'the ultimative answer' } )" | mongo


# read data from collection
printf "use pgtest\ndb.pgtesttable.find()" | mongo
