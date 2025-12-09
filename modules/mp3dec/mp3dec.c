#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "py/runtime.h"
#include "py/objstr.h"
#include "py/stream.h"
#include <string.h>

// Decoder state structure
typedef struct _mp3dec_obj_t {
    mp_obj_base_t base;
    mp3dec_t mp3d;
    mp3dec_frame_info_t info;
    mp_obj_t stream;      
    uint8_t *file_buf;    
    size_t file_buf_size;
    size_t buf_valid;     
} mp3dec_obj_t;

const mp_obj_type_t mp3dec_type;

// --- Constructor ---
static mp_obj_t mp3dec_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    mp3dec_obj_t *self = m_new_obj(mp3dec_obj_t);
    self->base.type = &mp3dec_type;
    
    mp3dec_init(&self->mp3d);
    self->stream = args[0];
    
    // Increased buffer size to 8KB to handle large ID3 tags better
    self->file_buf_size = 8192;
    self->file_buf = m_new(uint8_t, self->file_buf_size);
    self->buf_valid = 0;

    return MP_OBJ_FROM_PTR(self);
}

// --- Method: decode(output_buffer) ---
static mp_obj_t mp3dec_decode(mp_obj_t self_in, mp_obj_t out_buf_in) {
    mp3dec_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(out_buf_in, &bufinfo, MP_BUFFER_WRITE);
    short * pcm = (short *)bufinfo.buf;

    // THE FIX: Loop until we get samples OR run out of file
    while (1) {
        
        // 1. Refill Buffer if needed
        // We keep it nearly full to ensure we handle large frames/tags
        if (self->buf_valid < self->file_buf_size - 512) {
            size_t bytes_to_read = self->file_buf_size - self->buf_valid;
            
            // Move existing data to start
            if (self->buf_valid > 0) {
                memmove(self->file_buf, self->file_buf + (self->file_buf_size - bytes_to_read - self->buf_valid), self->buf_valid);
            }

            // Read from Python stream
            mp_obj_t read_method[2] = {
                mp_load_attr(self->stream, MP_QSTR_readinto), 
                mp_obj_new_bytearray_by_ref(bytes_to_read, self->file_buf + self->buf_valid)
            };
            mp_obj_t res = mp_call_method_n_kw(0, 0, read_method);
            size_t bytes_read = mp_obj_get_int(res);
            
            self->buf_valid += bytes_read;

            // If we read 0 bytes AND have no data left, we are truly done.
            if (bytes_read == 0 && self->buf_valid == 0) {
                return MP_OBJ_NEW_SMALL_INT(0); 
            }
        }

        // 2. Try to decode a frame
        int samples = mp3dec_decode_frame(&self->mp3d, self->file_buf, self->buf_valid, pcm, &self->info);
        
        // 3. Consume used bytes from input buffer
        size_t consumed = self->info.frame_bytes;
        self->buf_valid -= consumed;
        // Shift buffer pointer (virtual shift, real move happens at refill)
        memmove(self->file_buf, self->file_buf + consumed, self->buf_valid);

        // 4. CHECK: Did we actually get music?
        if (samples > 0) {
            // Yes! Return the number of bytes to Python
            return MP_OBJ_NEW_SMALL_INT(samples * self->info.channels * 2);
        }
        
        // If samples == 0, it means we parsed metadata/garbage. 
        // We Loop back to the top and Refill/Decode again immediately.
    }
}

// Wrapper for decode
static MP_DEFINE_CONST_FUN_OBJ_2(mp3dec_decode_obj, mp3dec_decode);


// --- Method: get_sample_rate() ---
static mp_obj_t mp3dec_get_sample_rate(mp_obj_t self_in) {
    mp3dec_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->info.hz);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp3dec_get_sample_rate_obj, mp3dec_get_sample_rate);


// --- Module Definitions ---

static const mp_rom_map_elem_t mp3dec_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_decode), MP_ROM_PTR(&mp3dec_decode_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_sample_rate), MP_ROM_PTR(&mp3dec_get_sample_rate_obj) },
};
static MP_DEFINE_CONST_DICT(mp3dec_locals_dict, mp3dec_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    mp3dec_type,
    MP_QSTR_MP3Decoder,
    MP_TYPE_FLAG_NONE,
    make_new, mp3dec_make_new,
    locals_dict, &mp3dec_locals_dict
);

static const mp_rom_map_elem_t mp3dec_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_mp3dec) },
    { MP_ROM_QSTR(MP_QSTR_MP3Decoder), MP_ROM_PTR(&mp3dec_type) },
};
static MP_DEFINE_CONST_DICT(mp3dec_globals, mp3dec_globals_table);

const mp_obj_module_t mp3dec_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp3dec_globals,
};

MP_REGISTER_MODULE(MP_QSTR_mp3dec, mp3dec_cmodule);
