module main

import vpg
import os

const test_data_dir = '/private/tmp/vpg_test_data'

fn testsuite_begin() ! {
	os.rmdir_all(test_data_dir) or {}
	vpg.initdb(test_data_dir, 'postgres')!
}

fn testsuite_end() ! {
	os.rmdir_all(test_data_dir) or {}
}

fn test_basic_crud() ! {
	mut pg := vpg.new_pg_embedded(test_data_dir, 'postgres', 'postgres')!
	defer {
		pg.close()
	}

	pg.query("CREATE TABLE IF NOT EXISTS users (id SERIAL PRIMARY KEY, name VARCHAR(100), email VARCHAR(100) UNIQUE);")!
	pg.query("CREATE TABLE IF NOT EXISTS posts (id SERIAL PRIMARY KEY, user_id INT REFERENCES users(id), title VARCHAR(200), content TEXT);")!
	pg.query("CREATE INDEX IF NOT EXISTS idx_posts_user_id ON posts(user_id);")!

	pg.query("INSERT INTO users (name, email) VALUES ('Alice', 'alice@example.com'), ('Bob', 'bob@example.com') ON CONFLICT (email) DO NOTHING;")!
	pg.query("INSERT INTO posts (user_id, title, content) VALUES (1, 'First Post', 'Hello world'), (2, 'Second Post', 'Vlang is great') ON CONFLICT DO NOTHING;")!

	res := pg.query("SELECT u.name, p.title FROM users u LEFT JOIN posts p ON p.user_id = u.id ORDER BY u.id;")!
	assert res.contains('Alice')
	assert res.contains('Bob')
	assert res.contains('First Post')

	pg.query("UPDATE users SET name = 'Alice Smith' WHERE email = 'alice@example.com';")!
	pg.query("DELETE FROM posts WHERE title = 'Second Post';")!

	res2 := pg.query("SELECT u.name, p.title FROM users u LEFT JOIN posts p ON p.user_id = u.id ORDER BY u.id;")!
	assert res2.contains('Alice Smith')
	assert !res2.contains('Second Post')
}
