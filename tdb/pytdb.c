/* 
   Unix SMB/CIFS implementation.

   Swig interface to tdb.

   Copyright (C) 2004-2006 Tim Potter <tpot@samba.org>
   Copyright (C) 2007-2008 Jelmer Vernooij <jelmer@samba.org>

     ** NOTE! The following LGPL license applies to the tdb
     ** library. This does NOT imply that all of Samba is released
     ** under the LGPL
   
   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#include <Python.h>
#ifdef HAVE_FSTAT
#undef HAVE_FSTAT
#endif

/* Include tdb headers */
#include <stdint.h>
#include <signal.h>
#include <tdb.h>
#include <fcntl.h>
#include <stdbool.h>

typedef struct {
	PyObject_HEAD
	TDB_CONTEXT *ctx;
	bool closed;
} PyTdbObject;

PyAPI_DATA(PyTypeObject) PyTdb;

static void PyErr_SetTDBError(TDB_CONTEXT *tdb)
{
	PyErr_SetObject(PyExc_RuntimeError, 
		Py_BuildValue("(i,s)", tdb_error(tdb), tdb_errorstr(tdb)));
}

static TDB_DATA PyString_AsTDB_DATA(PyObject *data)
{
	TDB_DATA ret;
	ret.dptr = (unsigned char *)PyString_AsString(data);
	ret.dsize = PyString_Size(data);
	return ret;
}

static PyObject *PyString_FromTDB_DATA(TDB_DATA data)
{
	if (data.dptr == NULL && data.dsize == 0) {
		return Py_None;
	} else {
		PyObject *ret = PyString_FromStringAndSize((const char *)data.dptr, 
												   data.dsize);
		free(data.dptr);
		return ret;
    }
}

#define PyErr_TDB_ERROR_IS_ERR_RAISE(ret, tdb) \
	if (ret != 0) { \
		PyErr_SetTDBError(tdb); \
		return NULL; \
	}

static PyObject *py_tdb_open(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
	char *name;
	int hash_size = 0, tdb_flags = TDB_DEFAULT, flags = O_RDWR, mode = 0600;
	TDB_CONTEXT *ctx;
	PyTdbObject *ret;
	const char *kwnames[] = { "name", "hash_size", "tdb_flags", "flags", "mode", NULL };

	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|iiii", (char **)kwnames, &name, &hash_size, &tdb_flags, &flags, &mode))
		return NULL;

	ctx = tdb_open(name, hash_size, tdb_flags, flags, mode);
	if (ctx == NULL) {
		PyErr_SetFromErrno(PyExc_IOError);
		return NULL;
	}

	ret = PyObject_New(PyTdbObject, &PyTdb);
	ret->ctx = ctx;
	ret->closed = false;
	return (PyObject *)ret;
}

static PyObject *obj_transaction_cancel(PyTdbObject *self)
{
	int ret = tdb_transaction_cancel(self->ctx);
	PyErr_TDB_ERROR_IS_ERR_RAISE(ret, self->ctx);
	return Py_None;
}

static PyObject *obj_transaction_commit(PyTdbObject *self)
{
	int ret = tdb_transaction_commit(self->ctx);
	PyErr_TDB_ERROR_IS_ERR_RAISE(ret, self->ctx);
	return Py_None;
}

static PyObject *obj_transaction_recover(PyTdbObject *self)
{
	int ret = tdb_transaction_recover(self->ctx);
	PyErr_TDB_ERROR_IS_ERR_RAISE(ret, self->ctx);
	return Py_None;
}

static PyObject *obj_transaction_start(PyTdbObject *self)
{
	int ret = tdb_transaction_start(self->ctx);
	PyErr_TDB_ERROR_IS_ERR_RAISE(ret, self->ctx);
	return Py_None;
}

static PyObject *obj_reopen(PyTdbObject *self)
{
	int ret = tdb_reopen(self->ctx);
	PyErr_TDB_ERROR_IS_ERR_RAISE(ret, self->ctx);
	return Py_None;
}

static PyObject *obj_lockall(PyTdbObject *self)
{
	int ret = tdb_lockall(self->ctx);
	PyErr_TDB_ERROR_IS_ERR_RAISE(ret, self->ctx);
	return Py_None;
}

static PyObject *obj_unlockall(PyTdbObject *self)
{
	int ret = tdb_unlockall(self->ctx);
	PyErr_TDB_ERROR_IS_ERR_RAISE(ret, self->ctx);
	return Py_None;
}

static PyObject *obj_lockall_read(PyTdbObject *self)
{
	int ret = tdb_lockall_read(self->ctx);
	PyErr_TDB_ERROR_IS_ERR_RAISE(ret, self->ctx);
	return Py_None;
}

static PyObject *obj_unlockall_read(PyTdbObject *self)
{
	int ret = tdb_unlockall_read(self->ctx);
	PyErr_TDB_ERROR_IS_ERR_RAISE(ret, self->ctx);
	return Py_None;
}

static PyObject *obj_close(PyTdbObject *self)
{
	int ret;
	if (self->closed)
		return Py_None;
	ret = tdb_close(self->ctx);
	self->closed = true;
	PyErr_TDB_ERROR_IS_ERR_RAISE(ret, self->ctx);
	return Py_None;
}

static PyObject *obj_get(PyTdbObject *self, PyObject *args)
{
	TDB_DATA key;
	PyObject *py_key;
	if (!PyArg_ParseTuple(args, "O", &py_key))
		return NULL;

	key = PyString_AsTDB_DATA(py_key);

	return PyString_FromTDB_DATA(tdb_fetch(self->ctx, key));
}

static PyObject *obj_append(PyTdbObject *self, PyObject *args)
{
	TDB_DATA key, data;
	PyObject *py_key, *py_data;
	int ret;
	if (!PyArg_ParseTuple(args, "OO", &py_key, &py_data))
		return NULL;

	key = PyString_AsTDB_DATA(py_key);
	data = PyString_AsTDB_DATA(py_data);

	ret = tdb_append(self->ctx, key, data);
	PyErr_TDB_ERROR_IS_ERR_RAISE(ret, self->ctx);
	return Py_None;
}

static PyObject *obj_firstkey(PyTdbObject *self)
{
	return PyString_FromTDB_DATA(tdb_firstkey(self->ctx));
}

static PyObject *obj_nextkey(PyTdbObject *self, PyObject *args)
{
	TDB_DATA key;
	PyObject *py_key;
	if (!PyArg_ParseTuple(args, "O", &py_key))
		return NULL;

	key = PyString_AsTDB_DATA(py_key);
	
	return PyString_FromTDB_DATA(tdb_nextkey(self->ctx, key));
}

static PyObject *obj_delete(PyTdbObject *self, PyObject *args)
{
	TDB_DATA key;
	PyObject *py_key;
	int ret;
	if (!PyArg_ParseTuple(args, "O", &py_key))
		return NULL;

	key = PyString_AsTDB_DATA(py_key);
	ret = tdb_delete(self->ctx, key);
	PyErr_TDB_ERROR_IS_ERR_RAISE(ret, self->ctx);
	return Py_None;
}

static PyObject *obj_has_key(PyTdbObject *self, PyObject *args)
{
	TDB_DATA key;
	int ret;
	PyObject *py_key;
	if (!PyArg_ParseTuple(args, "O", &py_key))
		return NULL;

	key = PyString_AsTDB_DATA(py_key);
	ret = tdb_exists(self->ctx, key);
	if (ret != TDB_ERR_NOEXIST) {
		PyErr_TDB_ERROR_IS_ERR_RAISE(ret, self->ctx);
	}

	return (ret == TDB_ERR_NOEXIST)?Py_False:Py_True;
}

static PyObject *obj_store(PyTdbObject *self, PyObject *args)
{
	TDB_DATA key, value;
	int ret;
	int flag = TDB_REPLACE;
	PyObject *py_key, *py_value;

	if (!PyArg_ParseTuple(args, "OO|i", &py_key, &py_value, &flag))
		return NULL;

	key = PyString_AsTDB_DATA(py_key);
	value = PyString_AsTDB_DATA(py_value);

	ret = tdb_store(self->ctx, key, value, flag);
	PyErr_TDB_ERROR_IS_ERR_RAISE(ret, self->ctx);
	return Py_None;
}


typedef struct {
	PyObject_HEAD
	TDB_DATA current;
	PyTdbObject *iteratee;
} PyTdbIteratorObject;

static PyObject *tdb_iter_next(PyTdbIteratorObject *self)
{
	TDB_DATA current;
	PyObject *ret;
	if (self->current.dptr == NULL && self->current.dsize == 0)
		return NULL;
	current = self->current;
	self->current = tdb_nextkey(self->iteratee->ctx, self->current);
	ret = PyString_FromTDB_DATA(current);
	return ret;
}

static void tdb_iter_dealloc(PyTdbIteratorObject *self)
{
	Py_DECREF(self->iteratee);
	PyObject_Del(self);
}

PyTypeObject PyTdbIterator = {
	.tp_name = "Iterator",
	.tp_basicsize = sizeof(PyTdbIteratorObject),
	.tp_iternext = (iternextfunc)tdb_iter_next,
	.tp_dealloc = (destructor)tdb_iter_dealloc,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_iter = PyObject_SelfIter,
};

static PyObject *tdb_object_iter(PyTdbObject *self)
{
	PyTdbIteratorObject *ret;	

	ret = PyObject_New(PyTdbIteratorObject, &PyTdbIterator);
	ret->current = tdb_firstkey(self->ctx);
	ret->iteratee = self;
	Py_INCREF(self);
	return (PyObject *)ret;
}

static PyObject *obj_clear(PyTdbObject *self)
{
	int ret = tdb_wipe_all(self->ctx);
	PyErr_TDB_ERROR_IS_ERR_RAISE(ret, self->ctx);
	return Py_None;
}

static PyMethodDef tdb_object_methods[] = {
	{ "transaction_cancel", (PyCFunction)obj_transaction_cancel, METH_NOARGS, 
		"S.transaction_cancel() -> None\n"
		"Cancel the currently active transaction." },
	{ "transaction_commit", (PyCFunction)obj_transaction_commit, METH_NOARGS,
		"S.transaction_commit() -> None\n"
		"Commit the currently active transaction." },
	{ "transaction_recover", (PyCFunction)obj_transaction_recover, METH_NOARGS,
		"S.transaction_recover() -> None\n"
		"Recover the currently active transaction." },
	{ "transaction_start", (PyCFunction)obj_transaction_start, METH_NOARGS,
		"S.transaction_start() -> None\n"
		"Start a new transaction." },
	{ "reopen", (PyCFunction)obj_reopen, METH_NOARGS, "Reopen this file." },
	{ "lock_all", (PyCFunction)obj_lockall, METH_NOARGS, NULL },
	{ "unlock_all", (PyCFunction)obj_unlockall, METH_NOARGS, NULL },
	{ "read_lock_all", (PyCFunction)obj_lockall_read, METH_NOARGS, NULL },
	{ "read_unlock_all", (PyCFunction)obj_unlockall_read, METH_NOARGS, NULL },
	{ "close", (PyCFunction)obj_close, METH_NOARGS, NULL },
	{ "get", (PyCFunction)obj_get, METH_VARARGS, "S.fetch(key) -> value\n"
		"Fetch a value." },
	{ "append", (PyCFunction)obj_append, METH_VARARGS, "S.append(key, value) -> None\n"
		"Append data to an existing key." },
	{ "firstkey", (PyCFunction)obj_firstkey, METH_NOARGS, "S.firstkey() -> data\n"
		"Return the first key in this database." },
	{ "nextkey", (PyCFunction)obj_nextkey, METH_NOARGS, "S.nextkey(key) -> data\n"
		"Return the next key in this database." },
	{ "delete", (PyCFunction)obj_delete, METH_VARARGS, "S.delete(key) -> None\n"
		"Delete an entry." },
	{ "has_key", (PyCFunction)obj_has_key, METH_VARARGS, "S.has_key(key) -> None\n"
		"Check whether key exists in this database." },
	{ "store", (PyCFunction)obj_store, METH_VARARGS, "S.store(key, data, flag=REPLACE) -> None"
		"Store data." },
	{ "iterkeys", (PyCFunction)tdb_object_iter, METH_NOARGS, "S.iterkeys() -> iterator" },
	{ "clear", (PyCFunction)obj_clear, METH_NOARGS, "S.clear() -> None\n"
		"Wipe the entire database." },
	{ NULL }
};

static PyObject *obj_get_hash_size(PyTdbObject *self, void *closure)
{
	return PyInt_FromLong(tdb_hash_size(self->ctx));
}

static int obj_set_max_dead(PyTdbObject *self, PyObject *max_dead, void *closure)
{
	if (!PyInt_Check(max_dead))
		return -1;
	tdb_set_max_dead(self->ctx, PyInt_AsLong(max_dead));
	return 0;
}

static PyObject *obj_get_map_size(PyTdbObject *self, void *closure)
{
	return PyInt_FromLong(tdb_map_size(self->ctx));
}

static PyObject *obj_get_flags(PyTdbObject *self, void *closure)
{
	return PyInt_FromLong(tdb_get_flags(self->ctx));
}

static PyObject *obj_get_filename(PyTdbObject *self, void *closure)
{
	return PyString_FromString(tdb_name(self->ctx));
}

static PyGetSetDef tdb_object_getsetters[] = {
	{ (char *)"hash_size", (getter)obj_get_hash_size, NULL, NULL },
	{ (char *)"map_size", (getter)obj_get_map_size, NULL, NULL },
	{ (char *)"flags", (getter)obj_get_flags, NULL, NULL },
	{ (char *)"max_dead", NULL, (setter)obj_set_max_dead, NULL },
	{ (char *)"filename", (getter)obj_get_filename, NULL, (char *)"The filename of this TDB file."},
	{ NULL }
};

static PyObject *tdb_object_repr(PyTdbObject *self)
{
	return PyString_FromFormat("Tdb('%s')", tdb_name(self->ctx));
}

static void tdb_object_dealloc(PyTdbObject *self)
{
	if (!self->closed)
		tdb_close(self->ctx);
	PyObject_Del(self);
}

static PyObject *obj_getitem(PyTdbObject *self, PyObject *key)
{
	TDB_DATA tkey, val;
	if (!PyString_Check(key)) {
		PyErr_SetString(PyExc_TypeError, "Expected string as key");
		return NULL;
	}

	tkey.dptr = (unsigned char *)PyString_AsString(key);
	tkey.dsize = PyString_Size(key);

	val = tdb_fetch(self->ctx, tkey);
	if (val.dptr == NULL) {
		PyErr_SetString(PyExc_KeyError, "No such TDB entry");
		return NULL;
	} else {
		return PyString_FromTDB_DATA(val);
	}
}

static int obj_setitem(PyTdbObject *self, PyObject *key, PyObject *value)
{
	TDB_DATA tkey, tval;
	int ret;
	if (!PyString_Check(key)) {
		PyErr_SetString(PyExc_TypeError, "Expected string as key");
		return -1;
	}

	tkey = PyString_AsTDB_DATA(key);

	if (value == NULL) { 
		ret = tdb_delete(self->ctx, tkey);
	} else { 
		if (!PyString_Check(value)) {
			PyErr_SetString(PyExc_TypeError, "Expected string as value");
			return -1;
		}

		tval = PyString_AsTDB_DATA(value);

		ret = tdb_store(self->ctx, tkey, tval, TDB_REPLACE);
	}

	if (ret != 0) {
		PyErr_SetTDBError(self->ctx);
		return -1;
	} 

	return ret;
}

static PyMappingMethods tdb_object_mapping = {
	.mp_subscript = (binaryfunc)obj_getitem,
	.mp_ass_subscript = (objobjargproc)obj_setitem,
};
PyTypeObject PyTdb = {
	.tp_name = "Tdb",
	.tp_basicsize = sizeof(PyTdbObject),
	.tp_methods = tdb_object_methods,
	.tp_getset = tdb_object_getsetters,
	.tp_new = py_tdb_open,
	.tp_doc = "A TDB file",
	.tp_repr = (reprfunc)tdb_object_repr,
	.tp_dealloc = (destructor)tdb_object_dealloc,
	.tp_as_mapping = &tdb_object_mapping,
	.tp_flags = Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE|Py_TPFLAGS_HAVE_ITER,
	.tp_iter = (getiterfunc)tdb_object_iter,
};

static PyMethodDef tdb_methods[] = {
	{ "open", (PyCFunction)py_tdb_open, METH_VARARGS|METH_KEYWORDS, "open(name, hash_size=0, tdb_flags=TDB_DEFAULT, flags=O_RDWR, mode=0600)\n"
		"Open a TDB file." },
	{ NULL }
};

void inittdb(void)
{
	PyObject *m;

	if (PyType_Ready(&PyTdb) < 0)
		return;

	if (PyType_Ready(&PyTdbIterator) < 0)
		return;

	m = Py_InitModule3("tdb", tdb_methods, "TDB is a simple key-value database similar to GDBM that supports multiple writers.");
	if (m == NULL)
		return;

	PyModule_AddObject(m, "REPLACE", PyInt_FromLong(TDB_REPLACE));
	PyModule_AddObject(m, "INSERT", PyInt_FromLong(TDB_INSERT));
	PyModule_AddObject(m, "MODIFY", PyInt_FromLong(TDB_MODIFY));

	PyModule_AddObject(m, "DEFAULT", PyInt_FromLong(TDB_DEFAULT));
	PyModule_AddObject(m, "CLEAR_IF_FIRST", PyInt_FromLong(TDB_CLEAR_IF_FIRST));
	PyModule_AddObject(m, "INTERNAL", PyInt_FromLong(TDB_INTERNAL));
	PyModule_AddObject(m, "NOLOCK", PyInt_FromLong(TDB_NOLOCK));
	PyModule_AddObject(m, "NOMMAP", PyInt_FromLong(TDB_NOMMAP));
	PyModule_AddObject(m, "CONVERT", PyInt_FromLong(TDB_CONVERT));
	PyModule_AddObject(m, "BIGENDIAN", PyInt_FromLong(TDB_BIGENDIAN));
	PyModule_AddObject(m, "__docformat__", PyString_FromString("restructuredText"));

	Py_INCREF(&PyTdb);
	PyModule_AddObject(m, "Tdb", (PyObject *)&PyTdb);

	Py_INCREF(&PyTdbIterator);
}
