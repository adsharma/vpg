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
#flag linux -latomic

fn C.vpg_initdb_options(data_dir &char, username &char, auth &char, encoding &char, locale &char, no_instructions bool)
fn C.vpg_backend_start_options(data_dir &char, username &char, dbname &char, shared_preload_libraries &char)
fn C.vpg_set_exec_path(path &char)
fn C.vpg_set_python_error(message &char)
fn C.vpg_python_error() &char
fn C.vpg_exec(query &char) &char
fn C.vpg_vacuum() int
fn C.vpg_analyze() int
fn C.vpg_maintain() int
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

pub struct PGOptions {
pub:
	data_dir                 string
	user                     string
	db                       string
	shared_preload_libraries string
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
	return new_pg_embedded_with_options(PGOptions{
		data_dir: data_dir
		user:     user
		db:       db
	})
}

pub fn new_pg_embedded_with_options(options PGOptions) !PGEmbedded {
	configure_exec_path()
	user := if options.user.len > 0 { options.user } else { 'postgres' }
	db := if options.db.len > 0 { options.db } else { user }
	if options.data_dir.len == 0 {
		return error('data directory is required')
	}
	if user.len == 0 {
		return error('user is required')
	}
	if db.len == 0 {
		return error('database name is required')
	}

	shared_preload_libraries := options.shared_preload_libraries
	C.vpg_backend_start_options(options.data_dir.str, user.str, db.str,
		shared_preload_libraries.str)
	if C.vpg_last_error_message() != 0 {
		return error(c_error_string())
	}
	return PGEmbedded{
		data_dir:    options.data_dir
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

pub fn (mut pg PGEmbedded) vacuum() ! {
	if !pg.initialized {
		return error('PG not initialized')
	}
	if C.vpg_vacuum() != 0 {
		return error(c_error_string())
	}
}

pub fn (mut pg PGEmbedded) analyze() ! {
	if !pg.initialized {
		return error('PG not initialized')
	}
	if C.vpg_analyze() != 0 {
		return error(c_error_string())
	}
}

pub fn (mut pg PGEmbedded) maintain() ! {
	if !pg.initialized {
		return error('PG not initialized')
	}
	if C.vpg_maintain() != 0 {
		return error(c_error_string())
	}
}

pub fn (mut pg PGEmbedded) close() {
	if !pg.initialized {
		return
	}
	C.vpg_finish()
	pg.initialized = false
}

fn py_cstring(value &char) string {
	if value == 0 {
		return ''
	}
	return unsafe { cstring_to_vstring(value) }
}

// The C backend is process-global today, so Python receives an opaque non-null
// token rather than a pointer to independent backend state.
@[export: 'vpg_py_set_exec_path']
@[py_export]
pub fn py_set_exec_path(path &char) {
	C.vpg_set_exec_path(path)
}

@[export: 'vpg_py_open']
@[py_export]
pub fn py_open(data_dir &char, user &char, db &char) voidptr {
	data_dir_v := py_cstring(data_dir)
	user_arg := py_cstring(user)
	db_arg := py_cstring(db)
	user_v := if user_arg.len > 0 { user_arg } else { 'postgres' }
	db_v := if db_arg.len > 0 { db_arg } else { user_v }
	if data_dir_v.len == 0 {
		C.vpg_set_python_error(c'data directory is required')
		return 0
	}
	C.vpg_backend_start_options(data_dir_v.str, user_v.str, db_v.str, c'')
	if C.vpg_last_error_message() != 0 {
		msg := C.vpg_last_error_message()
		if msg != 0 {
			C.vpg_set_python_error(msg)
		}
		return 0
	}
	return voidptr(1)
}

@[export: 'vpg_py_initdb']
@[py_export]
pub fn py_initdb(data_dir &char, user &char) int {
	data_dir_v := py_cstring(data_dir)
	user_arg := py_cstring(user)
	user_v := if user_arg.len > 0 { user_arg } else { 'postgres' }
	if data_dir_v.len == 0 {
		C.vpg_set_python_error(c'data directory is required')
		return -1
	}
	C.vpg_initdb_options(data_dir_v.str, user_v.str, c'trust', c'UTF8', c'C', true)
	if C.vpg_last_error_message() != 0 {
		msg := C.vpg_last_error_message()
		if msg != 0 {
			C.vpg_set_python_error(msg)
		}
		return -1
	}
	return 0
}

@[export: 'vpg_py_query']
@[py_export]
pub fn py_query(handle voidptr, query &char) &char {
	if handle == 0 {
		C.vpg_set_python_error(c'PG handle is null')
		return 0
	}
	c_result := C.vpg_exec(query)
	if c_result == 0 {
		msg := C.vpg_last_error_message()
		if msg != 0 {
			C.vpg_set_python_error(msg)
		}
		return 0
	}
	return c_result
}

fn py_maintenance(handle voidptr, action fn () int) int {
	if handle == 0 {
		C.vpg_set_python_error(c'PG handle is null')
		return -1
	}
	rc := action()
	if rc != 0 {
		msg := C.vpg_last_error_message()
		if msg != 0 {
			C.vpg_set_python_error(msg)
		}
	}
	return rc
}

@[export: 'vpg_py_vacuum']
@[py_export]
pub fn py_vacuum(handle voidptr) int {
	return py_maintenance(handle, C.vpg_vacuum)
}

@[export: 'vpg_py_analyze']
@[py_export]
pub fn py_analyze(handle voidptr) int {
	return py_maintenance(handle, C.vpg_analyze)
}

@[export: 'vpg_py_maintain']
@[py_export]
pub fn py_maintain(handle voidptr) int {
	return py_maintenance(handle, C.vpg_maintain)
}

@[export: 'vpg_py_close']
@[py_export]
pub fn py_close(handle voidptr) {
	if handle != 0 {
		C.vpg_finish()
	}
}

@[export: 'vpg_py_last_error']
@[py_export]
pub fn py_last_error() &char {
	return C.vpg_python_error()
}

@[export: 'vpg_py_free']
@[py_export]
pub fn py_free(ptr voidptr) {
	C.vpg_free(ptr)
}
