module main

import vpg

fn main() {
	mut pg := vpg.new_pg_embedded('./data', 'postgres', 'postgres') or {
		eprintln('init error: ${err}')
		exit(1)
	}
	defer {
		pg.close()
	}

	result := pg.query('SELECT current_database(), current_user;') or {
		eprintln('exec error: ${err}')
		exit(1)
	}

	print(result)
}
