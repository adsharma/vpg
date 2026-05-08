module vpg

import os

#include "@VMODROOT/vpg.h"

#flag -I @VMODROOT/postgresql/installed/include/server
#flag -I @VMODROOT/postgresql/installed/include/internal
#flag -I @VMODROOT/postgresql/src/include
#flag -I @VMODROOT/postgresql/src/backend
#flag -I @VMODROOT
#flag -L @VMODROOT
#flag -L @VMODROOT/postgresql/src/interfaces/libpq
#flag -L @VMODROOT/postgresql/installed/lib
#flag -L /opt/homebrew/opt/icu4c@78/lib
#flag linux -rdynamic
#flag @VMODROOT/libvpg.a
#flag @VMODROOT/postgresql/src/common/libpgcommon_srv.a
#flag @VMODROOT/postgresql/src/port/libpgport_srv.a
#flag -lpq
#flag -licui18n
#flag -licuuc
#flag -lz
#flag -lm

fn C.vpg_initdb(data_dir &char, username &char)
fn C.vpg_initdb_options(data_dir &char, username &char, auth &char, encoding &char, locale &char, no_instructions bool)
fn C.vpg_init(data_dir &char, username &char, dbname &char)
fn C.vpg_set_exec_path(path &char)
fn C.vpg_exec(query &char) &char
fn C.vpg_last_error_message() &char
fn C.vpg_finish()
fn C.vpg_free(ptr voidptr)

@[heap]
pub struct PGEmbedded {
pub:
	data_dir string
	user     string
	db       string
mut:
	initialized bool
}

pub struct InitDBOptions {
pub:
	user            string
	auth            string
	encoding        string
	locale          string
	no_instructions bool
}

fn c_error_string() string {
	msg := C.vpg_last_error_message()
	if msg == 0 {
		return 'unknown vpg error'
	}
	return unsafe { cstring_to_vstring(msg) }
}

fn configure_exec_path() {
	exec_path := os.executable()
	if exec_path.len > 0 {
		C.vpg_set_exec_path(exec_path.str)
	}
}

pub fn initdb(data_dir string, user string) ! {
	configure_exec_path()
	options := InitDBOptions{
		user:            user
		auth:            'trust'
		encoding:        'UTF8'
		locale:          'C'
		no_instructions: true
	}
	initdb_with_options(data_dir, options)!
}

pub fn initdb_with_options(data_dir string, options InitDBOptions) ! {
	configure_exec_path()
	user := if options.user.len > 0 { options.user } else { 'postgres' }
	auth := if options.auth.len > 0 { options.auth } else { 'trust' }
	encoding := if options.encoding.len > 0 { options.encoding } else { 'UTF8' }
	locale := if options.locale.len > 0 { options.locale } else { 'C' }

	C.vpg_initdb_options(data_dir.str, user.str, auth.str, encoding.str, locale.str,
		options.no_instructions)
	if C.vpg_last_error_message() != 0 {
		return error(c_error_string())
	}
}

pub fn new_pg_embedded(data_dir string, user string, db string) !PGEmbedded {
	configure_exec_path()
	C.vpg_init(data_dir.str, user.str, db.str)
	if C.vpg_last_error_message() != 0 {
		return error(c_error_string())
	}
	return PGEmbedded{
		data_dir:    data_dir
		user:        user
		db:          db
		initialized: true
	}
}

pub fn (mut pg PGEmbedded) query(query_text string) !string {
	if !pg.initialized {
		return error('PG not initialized')
	}

	c_result := C.vpg_exec(query_text.str)
	if c_result == 0 {
		return error(c_error_string())
	}
	defer {
		C.vpg_free(c_result)
	}
	return unsafe { cstring_to_vstring(c_result) }
}

pub fn (mut pg PGEmbedded) close() {
	if !pg.initialized {
		return
	}
	C.vpg_finish()
	pg.initialized = false
}
