DROP TABLE IF EXISTS test_01889;

CREATE TABLE test_01889 (x Int64)
ENGINE = MergeTree()
ORDER BY x;

INSERT INTO test_01889 SELECT * FROM numbers(1000);

SELECT count() FROM test_001889;

DELETE FROM test_01889 WHERE x % 100 != 0;

SELECT count() FROM test_001889;

DELETE FROM test_01889 WHERE x % 100 == 0;

SELECT count() FROM test_001889;

DELETE FROM test_01889 WHERE x > 1;

SELECT count() FROM test_001889;

DROP TABLE IF EXISTS test_01889;
