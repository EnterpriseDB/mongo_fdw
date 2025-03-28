// Cleanup of databases/collections created during regression run
// As 'test' is a default database, any foreign table created when
// database is not mentioned then corresponding collection gets
// created in test database. So dropping as part of cleanup.
use test
db.mongo_test3.drop();
use mongo_fdw_regress1
db.mongo_test1.drop();
use mongo_fdw_regress2
db.dropDatabase();
use mongo_fdw_regress
db.test_tbl1.drop();
db.test_tbl2.drop();
db.test_tbl3.drop();
db.test_tbl4.drop();
db.test_tbl5.drop();
db.test_tbl7.drop();
db.test_tbl8.drop();
db.test1.drop();
db.test2.drop();
db.test3.drop();
db.test4.drop();
db.mongo_test.drop();
db.test5.drop();
// Below queries will create and insert values in collections
db.mongo_test.insert({a : NumberInt(0), b : "mongo_test collection"});
db.test_tbl2.insertMany([
   {c1 : NumberInt(10), c2 : "DEVELOPMENT", c3 :"PUNE" },
   {c1: NumberInt(20), c2 : "ADMINISTRATION", c3 :"BANGLORE" },
   {c1: NumberInt(30), c2 : "SALES", c3 :"MUMBAI"  },
   {c1: NumberInt(40), c2 : "HR", c3 :"NAGPUR"  }
]);
db.test_tbl1.insertMany([
   {c1: NumberInt(100), c2 : "EMP1", c3 :"ADMIN", c4 :NumberInt(1300) ,c5 :ISODate("1980-12-17"), c6 :800.300, c7 :NumberInt(0), c8 :NumberInt(20) },
   {c1: NumberInt(200), c2 : "EMP2", c3 :"SALESMAN", c4 :NumberInt(600) ,c5 :ISODate("1981-02-20"), c6 :1600, c7 :NumberInt(300), c8 :NumberInt(30) },
   {c1: NumberInt(300), c2 : "EMP3", c3 :"SALESMAN", c4 :NumberInt(600) ,c5 :ISODate("1981-02-22"), c6 :1250, c7 :NumberInt(500), c8 :NumberInt(30)  },
   {c1: NumberInt(400), c2 : "EMP4", c3 :"MANAGER", c4 :NumberInt(900) ,c5 :ISODate("1981-04-02"), c6 :2975, c7 :NumberInt(0), c8 :NumberInt(20)  },
   {c1: NumberInt(500), c2 : "EMP5", c3 :"SALESMAN", c4 :NumberInt(600) ,c5 :ISODate("1981-09-28"), c6 :1250.23, c7 :NumberInt(1400), c8 :NumberInt(30)  },
   {c1: NumberInt(600), c2 : "EMP6", c3 :"MANAGER", c4 :NumberInt(900) ,c5 :ISODate("1981-05-01"), c6 :2850, c7 :NumberInt(0), c8 :NumberInt(30)  },
   {c1: NumberInt(700), c2 : "EMP7", c3 :"MANAGER", c4 :NumberInt(900) ,c5 :ISODate("1981-06-09"), c6 :2450.34, c7 :NumberInt(0), c8 :NumberInt(10)  },
   {c1: NumberInt(800), c2 : "EMP8", c3 :"FINANCE", c4 :NumberInt(400) ,c5 :ISODate("1987-04-19"), c6 :3000, c7 :NumberInt(0), c8 :NumberInt(20)  },
   {c1: NumberInt(900), c2 : "EMP9", c3 :"HEAD", c4 :null ,c5 :ISODate("1981-11-17"), c6 :5000, c7 :NumberInt(0), c8 :NumberInt(10)  },
   {c1: NumberInt(1000), c2 : "EMP10", c3 :"SALESMAN", c4 :NumberInt(600) ,c5 :ISODate("1980-09-08"), c6 :1500, c7 :NumberInt(0), c8 :NumberInt(30)  },
   {c1: NumberInt(1100), c2 : "EMP11", c3 :"ADMIN", c4 :NumberInt(800) ,c5 :ISODate("1987-05-23"), c6 :1100, c7 :NumberInt(0), c8 :NumberInt(20)  },
   {c1: NumberInt(1200), c2 : "EMP12", c3 :"ADMIN", c4 :NumberInt(600) ,c5 :ISODate("1981-12-03"), c6 :950.00, c7 :NumberInt(0), c8 :NumberInt(30)  },
   {c1: NumberInt(1300), c2 : "EMP13", c3 :"FINANCE", c4 :NumberInt(400) ,c5 :ISODate("1981-12-03"), c6 :3000, c7 :NumberInt(0), c8 :NumberInt(20) },
   {c1: NumberInt(1400), c2 : "EMP14", c3 :"ADMIN", c4 :NumberInt(700) ,c5 :ISODate("1982-01-23"), c6 :1300, c7 :NumberInt(0), c8 :NumberInt(10)  },
]);
db.test_tbl3.insertMany([
   {name: "dvd", marks: [23, 24], pass: false},
   {name: "vdd", marks: [29, 31], pass: true}
]);

db.test1.insertMany([
   {c1: NumberInt(1), c2: NumberInt(1), c3: "A"},
   {c1: NumberInt(2), c2: NumberInt(2), c3: "B"},
   {c1: NumberInt(3), c2: NumberInt(3), c3: "C"},
   {c1: NumberInt(4), c2: NumberInt(4), c3: "D"},
]);

db.test2.insertMany([
   {c1: NumberInt(5), c2: NumberInt(5), c3: "E"},
   {c1: NumberInt(6), c2: NumberInt(6), c3: "F"},
   {c1: NumberInt(7), c2: NumberInt(7), c3: "G"},
   {c1: NumberInt(8), c2: NumberInt(8), c3: "H"},
]);

db.test3.insertMany([
   {c1: NumberInt(1), c2: NumberInt(1), c3: "A"},
   {c1: NumberInt(2), c2: NumberInt(2), c3: "B"},
   {c1: NumberInt(3), c2: NumberInt(3), c3: "C"},
   {c1: NumberInt(4), c2: NumberInt(4), c3: "D"},
]);

db.test4.insertMany([
   {c1: NumberInt(5), c2: NumberInt(5), c3: "E"},
   {c1: NumberInt(6), c2: NumberInt(6), c3: "F"},
   {c1: NumberInt(7), c2: NumberInt(7), c3: "G"},
   {c1: NumberInt(8), c2: NumberInt(8), c3: "H"},
]);

db.test5.insertMany([
   {c1: 12.345678},
   {c1: -1.23}
]);
db.test_tbl4.insertMany([
   {a: NumberInt(25)},
   {a: NumberLong(9999999999)},
   {a: 25},
   {a: 25.09},
   {a: false}
]);
db.test_tbl5.insertMany([
   {a: NumberInt(25)},
   {a: 25},
   {a: 25.09},
   {a: true}
]);
db.test_tbl7.insertMany([
   {a: NumberInt(10), b: "ROW1"},
   {a: NumberInt(20), b: "ROW2"}
]);
db.test_tbl8.insertMany([
   {_id: NumberInt(1), a: NumberInt(2), b: "ROW1"},
   {a: NumberInt(3), b: "ROW2"},
]);
db.mongo_test_large.drop();
db.mongo_test_large.insertMany([
  {_id: NumberInt(0), a01 : NumberInt(1), a02 : NumberInt(2), a03 : NumberInt(3), a04 : NumberInt(4), a05 : NumberInt(5), a06 : NumberInt(6), a07 : NumberInt(7), a08 : NumberInt(8), a09 : NumberInt(9), a10 : NumberInt(10), a11 : NumberInt(11), a12 : NumberInt(12), a13 : NumberInt(13), a14 : NumberInt(14), a15 : NumberInt(15), a16 : NumberInt(16), a17 : NumberInt(17), a18 : NumberInt(18), a19 : NumberInt(19), a20 : NumberInt(20), a21 : NumberInt(21), a22 : NumberInt(22), a23 : NumberInt(23), a24 : NumberInt(24), a25 : NumberInt(25), a26 : NumberInt(26), a27 : NumberInt(27), a28 : NumberInt(28), a29 : NumberInt(29), a30 : NumberInt(30), a31 : NumberInt(31), a32 : NumberInt(32), a33 : NumberInt(33), a34 : NumberInt(134), a35 : NumberInt(35)},
  {_id: NumberInt(1), a01 : NumberInt(1), a02 : NumberInt(2), a03 : NumberInt(3), a04 : NumberInt(4), a05 : NumberInt(5), a06 : NumberInt(6), a07 : NumberInt(7), a08 : NumberInt(8), a09 : NumberInt(9), a10 : NumberInt(10), a11 : NumberInt(11), a12 : NumberInt(12), a13 : NumberInt(13), a14 : NumberInt(14), a15 : NumberInt(15), a16 : NumberInt(16), a17 : NumberInt(17), a18 : NumberInt(18), a19 : NumberInt(19), a20 : NumberInt(20), a21 : NumberInt(21), a22 : NumberInt(22), a23 : NumberInt(23), a24 : NumberInt(24), a25 : NumberInt(25), a26 : NumberInt(26), a27 : NumberInt(27), a28 : NumberInt(28), a29 : NumberInt(29), a30 : NumberInt(30), a31 : NumberInt(31), a32 : NumberInt(2), a33 : NumberInt(3), a34 : NumberInt(4), a35 : NumberInt(5)},
  {_id: NumberInt(2), a01 : NumberInt(1), a02 : NumberInt(2), a03 : NumberInt(3), a04 : NumberInt(4), a05 : NumberInt(5), a06 : NumberInt(6), a07 : NumberInt(7), a08 : NumberInt(8), a09 : NumberInt(9), a10 : NumberInt(10), a11 : NumberInt(11), a12 : NumberInt(12), a13 : NumberInt(13), a14 : NumberInt(14), a15 : NumberInt(15), a16 : NumberInt(16), a17 : NumberInt(17), a18 : NumberInt(18), a19 : NumberInt(19), a20 : NumberInt(20), a21 : NumberInt(21), a22 : NumberInt(22), a23 : NumberInt(23), a24 : NumberInt(24), a25 : NumberInt(25), a26 : NumberInt(26), a27 : NumberInt(27), a28 : NumberInt(28), a29 : NumberInt(29), a30 : NumberInt(30), a31 : NumberInt(31), a32 : NumberInt(132), a33 : NumberInt(133), a34 : NumberInt(134), a35 : NumberInt(135)},
  {_id: NumberInt(3), a01 : NumberInt(1), a02 : NumberInt(2), a03 : NumberInt(3), a04 : NumberInt(4), a05 : NumberInt(5), a06 : NumberInt(6), a07 : NumberInt(7), a08 : NumberInt(8), a09 : NumberInt(9), a10 : NumberInt(10), a11 : NumberInt(11), a12 : NumberInt(12), a13 : NumberInt(13), a14 : NumberInt(14), a15 : NumberInt(15), a16 : NumberInt(16), a17 : NumberInt(17), a18 : NumberInt(18), a19 : NumberInt(19), a20 : NumberInt(20), a21 : NumberInt(21), a22 : NumberInt(22), a23 : NumberInt(23), a24 : NumberInt(24), a25 : NumberInt(25), a26 : NumberInt(26), a27 : NumberInt(27), a28 : NumberInt(28), a29 : NumberInt(29), a30 : NumberInt(30), a31 : NumberInt(31), a32 : NumberInt(32), a33 : NumberInt(3), a34 : NumberInt(34), a35 : NumberInt(35)},
  {_id: NumberInt(4), a01 : NumberInt(1), a02 : NumberInt(2), a03 : NumberInt(3), a04 : NumberInt(4), a05 : NumberInt(5), a06 : NumberInt(6), a07 : NumberInt(7), a08 : NumberInt(8), a09 : NumberInt(9), a10 : NumberInt(10), a11 : NumberInt(11), a12 : NumberInt(12), a13 : NumberInt(13), a14 : NumberInt(14), a15 : NumberInt(15), a16 : NumberInt(16), a17 : NumberInt(17), a18 : NumberInt(18), a19 : NumberInt(19), a20 : NumberInt(20), a21 : NumberInt(21), a22 : NumberInt(22), a23 : NumberInt(23), a24 : NumberInt(24), a25 : NumberInt(25), a26 : NumberInt(26), a27 : NumberInt(27), a28 : NumberInt(28), a29 : NumberInt(29), a30 : NumberInt(30), a31 : NumberInt(31), a32 : NumberInt(32), a33 : NumberInt(33), a34 : NumberInt(34), a35 : NumberInt(35)}
]);
