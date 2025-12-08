#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "py/runtime.h"
#include "py/objstr.h"
#include "py/stream.h"

// Decoder state
typedef struct _mp3dec_obj_t {
    mp_obj_base_t base;
    mp3dec_t mp3d;
    mp3dec_frame_info_t info;
    mp_obj_t stream;      // The file object
    uint8_t *file_buf;    // Buffer for reading from file
    size_t file_buf_size;
    size_t buf_valid;     // How many bytes are valid in file_buf
} mp3dec_obj_t;

const mp_obj_type_t mp3dec_type;

// Constructor: MP3Decoder(file_stream)
STATIC mp_obj_t mp3dec_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    mp3dec_obj_t *self = m_new_obj(mp3dec_obj_t);
    self->base.type = &mp3dec_type;
    
    mp3dec_init(&self->mp3d);
    self->stream = args[0];
    
    // Allocate a small buffer for reading the MP3 file chunks
    // MP3 frames are rarely larger than 2KB, so 4KB is safe
    self->file_buf_size = 4096;
    self->file_buf = m_new(uint8_t, self->file_buf_size);
    self->buf_valid = 0;

    return MP_OBJ_FROM_PTR(self);
}

// Method: decode(output_buffer) -> bytes_written
STATIC mp_obj_t mp3dec_decode(mp_obj_t self_in, mp_obj_t out_buf_in) {
    mp3dec_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(out_buf_in, &bufinfo, MP_BUFFER_WRITE);
    short * pcm = (short *)bufinfo.buf;
    size_t max_samples = bufinfo.len / 2; // 16-bit samples

    // Read more data from file if we are running low
    if (self->buf_valid < 1024) {
        // Move remaining data to start
        memmove(self->file_buf, self->file_buf + (self->file_buf_size - self->buf_valid), self->buf_valid);
        
        // Read from stream
        int err;
        mp_obj_t read_method[2] = {
            mp_load_attr(self->stream, MP_QSTR_readinto), 
            mp_obj_new_bytearray_by_ref(self->file_buf_size - self->buf_valid, self->file_buf + self->buf_valid)
        };
        mp_obj_t res = mp_call_method_n_kw(0, 0, read_method);
        size_t bytes_read = mp_obj_get_int(res);
        self->buf_valid += bytes_read;
    }

    if (self->buf_valid == 0) return MP_OBJ_NEW_SMALL_INT(0); // EOF

    int samples = mp3dec_decode_frame(&self->mp3d, self->file_buf, self->buf_valid, pcm, &self->info);
    
    // Consume used bytes from input buffer
    size_t consumed = self->info.frame_bytes;
    self->buf_valid -= consumed;
    memmove(self->file_buf, self->file_buf + consumed, self->buf_valid);

    return MP_OBJ_NEW_SMALL_INT(samples * self->info.channels * 2); // Return bytes written
}

// Method: get_sample_rate()
STATIC mp_obj_t mp3dec_get_sample_rate(mp_obj_t self_in) {
    mp3dec_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->info.hz);
}

STATIC const mp_rom_map_elem_t mp3dec_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_decode), MP_ROM_PTR(&mp3dec_decode_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_sample_rate), MP_ROM_PTR(&mp3dec_get_sample_rate_obj) },
};
STATIC MP_DEFINE_CONST_DICT(mp3dec_locals_dict, mp3dec_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    mp3dec_type,
    MP_QSTR_MP3Decoder,
    MP_TYPE_FLAG_NONE,
    make_new, mp3dec_make_new,
    locals_dict, &mp3dec_locals_dict
);

STATIC const mp_rom_map_elem_t mp3dec_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_mp3dec) },
    { MP_ROM_QSTR(MP_QSTR_MP3Decoder), MP_ROM_PTR(&mp3dec_type) },
};
STATIC MP_DEFINE_CONST_DICT(mp3dec_globals, mp3dec_globals_table);

const mp_obj_module_t mp3dec_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp3dec_globals,
};

MP_REGISTER_MODULE(MP_QSTR_mp3dec, mp3dec_cmodule);