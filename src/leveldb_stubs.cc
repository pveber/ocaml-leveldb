
#include <leveldb/db.h>
#include <leveldb/write_batch.h>

extern "C" {

#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/alloc.h>
#include <caml/fail.h>
#include <caml/callback.h>
#include <caml/custom.h>
#include <caml/signals.h>
#include <string.h>

typedef struct ldb_any {
  void *data;
  void (*release)(void *);
} ldb_any;


typedef struct { leveldb::DB *db; } ldb_handle;
typedef struct { leveldb::Iterator *it; } ldb_iterator;
typedef struct { leveldb::WriteBatch *batch; } ldb_writebatch;

static void ldb_any_finalize(value t);
static int ldb_any_compare(value t1, value t2);
static long ldb_any_hash(value t);

#define LDB_ANY(x) ((ldb_any *) Data_custom_val(x))

#define LDB_HANDLE(x) (((ldb_handle *) Data_custom_val(x))->db)
#define LDB_ITERATOR(x) (((ldb_iterator *) Data_custom_val(x))->it)
#define LDB_WRITEBATCH(x) (((ldb_writebatch *) Data_custom_val(x))->batch)

#define WRAP(_dst, _data, _type) \
  do { \
      leveldb::_type *p = _data; \
      _dst = caml_alloc_custom(&ldb_any_ops, sizeof(ldb_any), 0, 1); \
      LDB_ANY(_dst)->data = p; \
      LDB_ANY(_dst)->release = (void (*)(void *))release_##_type; \
  } while(0);

static struct custom_operations ldb_any_ops = {
    (char *)"org.eigenclass/leveldb_any",
    ldb_any_finalize,
    ldb_any_compare,
    ldb_any_hash,
    custom_serialize_default,
    custom_deserialize_default
};

static value *not_found_exn = 0;
static value *error_exn = 0;

static void raise_error(const char *s)
{
 if(!error_exn) error_exn = caml_named_value("org.eigenclass/leveldb/Error");
 if(!error_exn)
     caml_failwith(s);
 else
     caml_raise_with_string(*error_exn, s);
}

#define RAISE_NOT_FOUND \
    do { \
	if(!not_found_exn) \
	    not_found_exn = caml_named_value("org.eigenclass/leveldb/Not_found"); \
	if(!not_found_exn) \
	    caml_failwith("Not_found"); \
	else \
	    caml_raise_constant(*not_found_exn); \
    } while(0);

#define CHECK_ERROR(status) \
    do { \
      if(!status.ok()) raise_error(status.ToString().c_str()); \
    } while(0);

#define CHECK_CLOSED(t) \
    do { \
      if(!LDB_HANDLE(t)) raise_error("leveldb handle closed"); \
    } while(0);

#define CHECK_IT_CLOSED(_it) \
    do { \
      if(!LDB_ITERATOR(_it)) raise_error("iterator closed"); \
    } while(0);

static void
ldb_any_finalize(value t)
{
 ldb_any *h = LDB_ANY(t);
 if(h->data) {
     h->release(h->data);
     h->data = NULL;
 }
}

static int
ldb_any_compare(value t1, value t2)
{
 ldb_any *h1, *h2;
 h1 = LDB_ANY(t1);
 h2 = LDB_ANY(t2);
 return ((char*)h1->data - (char *)h2->data);
}

static long
ldb_any_hash(value t)
{
 return (long)LDB_ANY(t)->data;
}

static void release_DB(leveldb::DB *db)
{
 delete db;
}

static void release_Iterator(leveldb::Iterator *iterator)
{
 delete iterator;
}

static void release_WriteBatch(leveldb::WriteBatch *writebatch)
{
 delete writebatch;
}

CAMLprim value
ldb_open(value s, value write_buffer_size, value max_open_files,
         value block_size, value block_restart_interval)
{
 CAMLparam1(s);
 CAMLlocal1(r);

 leveldb::Options options;

 leveldb::DB* db;
 options.create_if_missing = true;
 options.write_buffer_size = Long_val(write_buffer_size);
 options.max_open_files = Int_val(max_open_files);
 options.block_size = Int_val(block_size);
 options.block_restart_interval = Int_val(block_restart_interval);
 leveldb::Status status = leveldb::DB::Open(options, String_val(s), &db);
 CHECK_ERROR(status);

 WRAP(r, db, DB);
 CAMLreturn(r);
}

CAMLprim value
ldb_close(value t)
{
  ldb_any_finalize(t);
  return(Val_unit);
}

CAMLprim value
ldb_destroy(value s)
{
 std::string _s(String_val(s), string_length(s));

 caml_enter_blocking_section();
 leveldb::Status status = leveldb::DestroyDB(_s, leveldb::Options());
 caml_leave_blocking_section();
 return(status.ok() ? Val_true : Val_false);
}

CAMLprim value
ldb_repair(value s)
{
 std::string _s(String_val(s), string_length(s));

 caml_enter_blocking_section();
 leveldb::Status status = leveldb::RepairDB(_s, leveldb::Options());
 caml_leave_blocking_section();
 return(status.ok() ? Val_true : Val_false);
}

#define TO_SLICE(x) leveldb::Slice(String_val(x), string_length(x))
#define COPY_FROM(dst, src) \
    do { \
        dst = caml_alloc_string(src.size()); \
        memcpy(String_val(dst), src.data(), src.size()); \
    } while(0);

CAMLprim value
ldb_get(value t, value k)
{
 CAMLparam2(t, k);
 CAMLlocal1(ret);
 leveldb::DB *db = LDB_HANDLE(t);

 CHECK_CLOSED(t);
 leveldb::Slice key = TO_SLICE(k);
 std::string v;
 leveldb::Status status = db->Get(leveldb::ReadOptions(), key, &v);
 if(status.IsNotFound()) { RAISE_NOT_FOUND; }

 CHECK_ERROR(status);

 COPY_FROM(ret, v);
 CAMLreturn(ret);
}

CAMLprim value
ldb_put(value t, value k, value v, value sync)
{
 CAMLparam3(t, k, v);
 leveldb::DB *db = LDB_HANDLE(t);

 CHECK_CLOSED(t);
 leveldb::Slice key = TO_SLICE(k);
 leveldb::Slice val = TO_SLICE(v);
 leveldb::WriteOptions options;
 options.sync = (Val_true == sync);

 leveldb::Status status = db->Put(options, key, val);

 CHECK_ERROR(status);
 CAMLreturn(Val_unit);
}


CAMLprim value
ldb_delete(value t, value k, value sync)
{
 CAMLparam2(t, k);
 leveldb::DB *db = LDB_HANDLE(t);

 CHECK_CLOSED(t);
 leveldb::Slice key = TO_SLICE(k);
 leveldb::WriteOptions options;
 options.sync = (Val_true == sync);

 leveldb::Status status = db->Delete(options, key);
 CHECK_ERROR(status);

 CAMLreturn(Val_unit);
}

CAMLprim value
ldb_mem(value t, value k)
{
 CAMLparam2(t, k);
 CAMLlocal1(ret);
 leveldb::DB *db = LDB_HANDLE(t);

 CHECK_CLOSED(t);
 leveldb::Slice key = TO_SLICE(k);
 std::string v;
 leveldb::Status status = db->Get(leveldb::ReadOptions(), key, &v);
 bool not_found;

 CHECK_ERROR(status);

 not_found = status.IsNotFound();

 CAMLreturn(not_found ? Val_false : Val_true);
}

CAMLprim value
ldb_make_iter(value t)
{
 CAMLparam1(t);
 CAMLlocal1(it);

 leveldb::DB *db = LDB_HANDLE(t);

 CHECK_CLOSED(t);
 leveldb::Iterator *_it = db->NewIterator(leveldb::ReadOptions());

 WRAP(it, _it, Iterator);
 CAMLreturn(it);
}

CAMLprim value
ldb_iter_close(value t)
{
 ldb_any_finalize(t);
 return(Val_unit);
}

CAMLprim value
ldb_it_first(value it)
{
 CAMLparam1(it);
 CHECK_IT_CLOSED(it);
 leveldb::Iterator *_it = LDB_ITERATOR(it);

 _it->SeekToFirst();
 CAMLreturn(Val_unit);
}

CAMLprim value
ldb_it_last(value it)
{
 CAMLparam1(it);
 CHECK_IT_CLOSED(it);
 leveldb::Iterator *_it = LDB_ITERATOR(it);

 _it->SeekToLast();
 CAMLreturn(Val_unit);
}

CAMLprim value
ldb_it_seek_unsafe(value it, value s, value off, value len)
{
 CAMLparam2(it, s);
 CHECK_IT_CLOSED(it);
 leveldb::Iterator *_it = LDB_ITERATOR(it);

 leveldb::Slice key(String_val(s) + Int_val(off), Int_val(len));
 _it->Seek(key);
 CAMLreturn(Val_unit);
}

CAMLprim value
ldb_it_next(value it)
{
 CAMLparam1(it);
 CHECK_IT_CLOSED(it);
 leveldb::Iterator *_it = LDB_ITERATOR(it);

 _it->Next();
 CAMLreturn(Val_unit);
}

CAMLprim value
ldb_it_prev(value it)
{
 CAMLparam1(it);
 CHECK_IT_CLOSED(it);
 leveldb::Iterator *_it = LDB_ITERATOR(it);

 _it->Prev();
 CAMLreturn(Val_unit);
}

CAMLprim value
ldb_it_valid(value it)
{
 leveldb::Iterator *_it = LDB_ITERATOR(it);

 if(!_it || !_it->Valid()) return Val_false;

 return Val_true;
}

/* returns:
 * SIZE if the key exists and its size is SIZE
 * if SIZE <= buf len, the key is copied into the supplied buffer
 */
CAMLprim value
ldb_it_key_unsafe(value it, value buf)
{
 CAMLparam2(it, buf);

 CHECK_IT_CLOSED(it);
 leveldb::Iterator *_it = LDB_ITERATOR(it);

 if(!_it->Valid()) raise_error(_it->status().ToString().c_str());

 leveldb::Slice key = _it->key();
 size_t size = key.size();

 if(size <= string_length(buf))
     memcpy(String_val(buf), key.data(), size);

 CAMLreturn(Val_long(size));
}

/* returns:
 * SIZE if the value exists and its size is SIZE
 * if SIZE <= buf len, the value is copied into the supplied buffer
 */
CAMLprim value
ldb_it_value_unsafe(value it, value buf)
{
 CAMLparam2(it, buf);

 CHECK_IT_CLOSED(it);
 leveldb::Iterator *_it = LDB_ITERATOR(it);

 if(!_it->Valid()) raise_error(_it->status().ToString().c_str());

 leveldb::Slice v = _it->value();
 size_t size = v.size();

 if(size <= string_length(buf))
     memcpy(String_val(buf), v.data(), size);

 CAMLreturn(Val_long(size));
}

CAMLprim value
ldb_iter(value f, value t)
{
 CAMLparam2(t, f);
 CAMLlocal3(it, k, v);
 leveldb::DB *db = LDB_HANDLE(t);

 CHECK_CLOSED(t);
 leveldb::Iterator *_it = db->NewIterator(leveldb::ReadOptions());

 WRAP(it, _it, Iterator);

 for(_it->SeekToFirst(); _it->Valid(); _it->Next()) {
     COPY_FROM(k, _it->key());
     COPY_FROM(v, _it->value());
     if(caml_callback2(f, k, v) == Val_false) break;
 }
 CAMLreturn(Val_unit);
}

CAMLprim value
ldb_iter_from(value f, value t, value start)
{
 CAMLparam2(t, f);
 CAMLlocal3(it, k, v);
 leveldb::DB *db = LDB_HANDLE(t);

 CHECK_CLOSED(t);
 leveldb::Iterator *_it = db->NewIterator(leveldb::ReadOptions());
 WRAP(it, _it, Iterator);

 for(_it->Seek(TO_SLICE(start)); _it->Valid(); _it->Next()) {
     COPY_FROM(k, _it->key());
     COPY_FROM(v, _it->value());
     if(caml_callback2(f, k, v) == Val_false) break;
 }
 CAMLreturn(Val_unit);
}

CAMLprim value
ldb_rev_iter(value f, value t)
{
 CAMLparam2(t, f);
 CAMLlocal3(it, k, v);
 leveldb::DB *db = LDB_HANDLE(t);

 CHECK_CLOSED(t);
 leveldb::Iterator *_it = db->NewIterator(leveldb::ReadOptions());
 WRAP(it, _it, Iterator);

 for(_it->SeekToLast(); _it->Valid(); _it->Prev()) {
     COPY_FROM(k, _it->key());
     COPY_FROM(v, _it->value());
     if(caml_callback2(f, k, v) == Val_false) break;
 }
 CAMLreturn(Val_unit);
}

CAMLprim value
ldb_rev_iter_from(value t, value f, value start)
{
 CAMLparam2(t, f);
 CAMLlocal3(it, k, v);
 leveldb::DB *db = LDB_HANDLE(t);

 CHECK_CLOSED(t);
 leveldb::Iterator *_it = db->NewIterator(leveldb::ReadOptions());
 WRAP(it, _it, Iterator);

 for(_it->Seek(TO_SLICE(start)); _it->Valid(); _it->Prev()) {
     COPY_FROM(k, _it->key());
     COPY_FROM(v, _it->value());
     if(caml_callback2(f, k, v) == Val_false) break;
 }
 CAMLreturn(Val_unit);
}

CAMLprim value
ldb_writebatch_make(value unit)
{
 CAMLparam0();
 CAMLlocal1(ret);

 leveldb::WriteBatch *b = new leveldb::WriteBatch;
 WRAP(ret, b, WriteBatch);

 CAMLreturn(ret);
}

CAMLprim value
ldb_writebatch_put(value t, value k, value v)
{
 leveldb::WriteBatch *b = LDB_WRITEBATCH(t);

 leveldb::Slice key = TO_SLICE(k);
 leveldb::Slice value = TO_SLICE(v);
 b->Put(key, value);

 return Val_unit;
}

CAMLprim value
ldb_writebatch_delete(value t, value k)
{
 leveldb::WriteBatch *b = LDB_WRITEBATCH(t);

 leveldb::Slice key = TO_SLICE(k);
 b->Delete(key);

 return Val_unit;
}

CAMLprim value
ldb_write_batch(value t, value batch, value sync)
{
 CAMLparam2(t, batch);
 leveldb::DB *db = LDB_HANDLE(t);
 leveldb::WriteBatch *b = LDB_WRITEBATCH(batch);

 CHECK_CLOSED(t);
 leveldb::WriteOptions options;
 options.sync = (Val_true == sync);

 leveldb::Status status = db->Write(options, b);

 CHECK_ERROR(status);
 CAMLreturn(Val_unit);
}

CAMLprim value
ldb_get_approximate_size(value t, value _from, value _to)
{
 CAMLparam3(t, _from, _to);
 CAMLlocal1(ret);
 leveldb::DB *db = LDB_HANDLE(t);

 CHECK_CLOSED(t);
 leveldb::Range range(TO_SLICE(_from), TO_SLICE(_to));
 uint64_t size;
 db->GetApproximateSizes(&range, 1, &size);

 ret = caml_copy_int64(size);
 CAMLreturn(ret);
}

}