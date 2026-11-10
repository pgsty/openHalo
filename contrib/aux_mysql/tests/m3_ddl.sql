/* Copyright (c) 2026 PGSTY PTE. LTD. */ CREATE TABLE `users` (
  `id` bigint NOT NULL AUTO_INCREMENT COMMENT 'pk',
  `name` varchar(64) NOT NULL DEFAULT '',
  `email` varchar(128) DEFAULT NULL,
  `age` int DEFAULT 0,
  `created_at` datetime DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_email` (`email`),
  KEY `idx_name` (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='user table';
INSERT INTO users (name, email, age) VALUES ('alice', 'a@x.com', 30), ('bob', 'b@x.com', 25);
INSERT INTO users (name, email, age) VALUES ('alice2', 'a@x.com', 31) ON DUPLICATE KEY UPDATE age = VALUES(age);
REPLACE INTO users (id, name, email, age) VALUES (2, 'bobby', 'b@x.com', 26);
INSERT IGNORE INTO users (id, name) VALUES (1, 'dup');
SELECT * FROM users;
SELECT LAST_INSERT_ID();
ALTER TABLE users MODIFY COLUMN age bigint NOT NULL DEFAULT 1;
ALTER TABLE users DROP INDEX idx_name;
SHOW TABLES;
SET @a = 1;
SELECT @a + 1;
SET NAMES utf8mb4;
SET autocommit = 1;
SELECT @@version, @@version_comment;
