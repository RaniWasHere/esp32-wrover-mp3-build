#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "py/runtime.h"
#include "py/objstr.h"
#include "py/stream.h"
#include <string.h>

// --- Object Structure ---
typedef struct _mp3dec_obj_t {
    mp_obj_base_t base;
    mp3dec_t mp3d;
    mp3dec_frame_info_t info;
    mp_obj_t stream;      
    uint8_t *file_buf;    
    size_t file_buf_size;
    size_t buf_valid;
    int volume;
    float current_sec; // Track playback time
    bool force_mono;   // New: Force stereo to mono mix
} mp3dec_obj_t;

const mp_obj_type_t mp3dec_type;

// --- Constructor ---
// Usage: MP3Decoder(stream, buf_size=8192)
static mp_obj_t mp3dec_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 2, false); // Allow 1 or 2 args
    
    mp3dec_obj_t *self = m_new_obj(mp3dec_obj_t);
    self->base.type = &mp3dec_type;
    
    mp3dec_init(&self->mp3d);
    self->stream = args[0];
    
    // Configurable buffer size (Default 8KB)
    self->file_buf_size = (n_args > 1) ? mp_obj_get_int(args[1]) : 8192;
    if (self->file_buf_size < 1024) self->file_buf_size = 1024; // Safety minimum
    
    self->file_buf = m_new(uint8_t, self->file_buf_size);
    self->buf_valid = 0;
    self->volume = 100;
    self->current_sec = 0.0f;
    self->force_mono = false;

    return MP_OBJ_FROM_PTR(self);
}

// --- Method: decode ---
static mp_obj_t mp3dec_decode(mp_obj_t self_in, mp_obj_t out_buf_in) {
    mp3dec_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(out_buf_in, &bufinfo, MP_BUFFER_WRITE);
    short * pcm = (short *)bufinfo.buf;

    while (1) {
        // 1. Refill Buffer if needed
        if (self->buf_valid < self->file_buf_size - 512) { // 512 is safe margin for headers
            size_t bytes_to_read = self->file_buf_size - self->buf_valid;
            
            // Shift remaining data to start
            if (self->buf_valid > 0) {
                memmove(self->file_buf, self->file_buf + (self->file_buf_size - bytes_to_read - self->buf_valid), self->buf_valid);
            }
            
            // Read from Python Stream
            mp_obj_t read_method[2] = {
                mp_load_attr(self->stream, MP_QSTR_readinto), 
                mp_obj_new_bytearray_by_ref(bytes_to_read, self->file_buf + self->buf_valid)
            };
            mp_obj_t res = mp_call_method_n_kw(0, 0, read_method);
            size_t bytes_read = mp_obj_get_int(res);
            self->buf_valid += bytes_read;
            
            // End of File
            if (bytes_read == 0 && self->buf_valid == 0) return MP_OBJ_NEW_SMALL_INT(0); 
        }

        // 2. Decode Frame
        int samples = mp3dec_decode_frame(&self->mp3d, self->file_buf, self->buf_valid, pcm, &self->info);
        
        // 3. Consume Bytes
        size_t consumed = self->info.frame_bytes;
        if (consumed == 0) consumed = 1; // Prevent infinite loop on bad data
        if (consumed > self->buf_valid) consumed = self->buf_valid; // Safety

        self->buf_valid -= consumed;
        memmove(self->file_buf, self->file_buf + consumed, self->buf_valid);

        if (samples > 0) {
            // Update internal timer
            if (self->info.hz > 0) {
                self->current_sec += (float)samples / (float)self->info.hz;
            }

            int output_samples = samples * self->info.channels;

            // 4. Post-Processing: Volume & Mono Mixing
            // Optimization: Combine loops if volume != 100
            if (self->force_mono && self->info.channels == 2) {
                // Mix Stereo -> Mono (Average L+R)
                for (int i = 0; i < samples; i++) {
                    int32_t mixed = ((int32_t)pcm[i*2] + (int32_t)pcm[i*2+1]) / 2;
                    if (self->volume < 100) mixed = mixed * self->volume / 100;
                    pcm[i] = (short)mixed; // Store continuously
                }
                return MP_OBJ_NEW_SMALL_INT(samples * 2); // Return bytes (samples * 1 channel * 2 bytes)
            } 
            else if (self->volume < 100) {
                // Just Volume
                for (int i = 0; i < output_samples; i++) {
                    pcm[i] = (short)((int32_t)pcm[i] * self->volume / 100);
                }
            }

            // Return number of bytes written to PCM buffer
            // (Samples * Channels * 2 bytes_per_short)
            return MP_OBJ_NEW_SMALL_INT(output_samples * 2);
        }
    }
}
static MP_DEFINE_CONST_FUN_OBJ_2(mp3dec_decode_obj, mp3dec_decode);

// --- Method: seek ---
// Allows jumping to a specific second. Warning: For VBR files this is an approximation.
// Python does the math; C just executes the move.
static mp_obj_t mp3dec_seek(mp_obj_t self_in, mp_obj_t byte_offset_in, mp_obj_t time_sec_in) {
    mp3dec_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    int offset = mp_obj_get_int(byte_offset_in);
    float new_time = mp_obj_get_float(time_sec_in);

    // 1. Call Python stream.seek(offset)
    mp_obj_t seek_method[3] = {
        mp_load_attr(self->stream, MP_QSTR_seek),
        mp_obj_new_int(offset),
        mp_obj_new_int(0) // 0 = SEEK_SET (absolute)
    };
    mp_call_method_n_kw(0, 0, seek_method);

    // 2. Reset Decoder State
    // We must flush internal buffers so we don't play old data
    self->buf_valid = 0;
    mp3dec_init(&self->mp3d); 
    
    // 3. Update the internal timer to what Python told us
    self->current_sec = new_time;

    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_3(mp3dec_seek_obj, mp3dec_seek);

// --- Method: tell ---
// Returns current playback position in seconds
static mp_obj_t mp3dec_tell(mp_obj_t self_in) {
    mp3dec_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_float(self->current_sec);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp3dec_tell_obj, mp3dec_tell);

// --- Settings ---
static mp_obj_t mp3dec_set_volume(mp_obj_t self_in, mp_obj_t vol_in) {
    mp3dec_obj_t *self = MP_OBJ_TO_PTR(self_in);
    int vol = mp_obj_get_int(vol_in);
    self->volume = (vol < 0) ? 0 : (vol > 100 ? 100 : vol);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mp3dec_set_volume_obj, mp3dec_set_volume);

static mp_obj_t mp3dec_set_mono(mp_obj_t self_in, mp_obj_t enable_in) {
    mp3dec_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->force_mono = mp_obj_is_true(enable_in);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mp3dec_set_mono_obj, mp3dec_set_mono);

// --- Getters ---
static mp_obj_t mp3dec_get_sample_rate(mp_obj_t self_in) {
    mp3dec_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->info.hz);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp3dec_get_sample_rate_obj, mp3dec_get_sample_rate);

static mp_obj_t mp3dec_get_bitrate(mp_obj_t self_in) {
    mp3dec_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->info.bitrate_kbps);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp3dec_get_bitrate_obj, mp3dec_get_bitrate);

static mp_obj_t mp3dec_get_channels(mp_obj_t self_in) {
    mp3dec_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->info.channels);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp3dec_get_channels_obj, mp3dec_get_channels);


// --- Module Map ---
static const mp_rom_map_elem_t mp3dec_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_decode), MP_ROM_PTR(&mp3dec_decode_obj) },
    { MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&mp3dec_seek_obj) },        // New
    { MP_ROM_QSTR(MP_QSTR_tell), MP_ROM_PTR(&mp3dec_tell_obj) },        // New
    { MP_ROM_QSTR(MP_QSTR_set_volume), MP_ROM_PTR(&mp3dec_set_volume_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_mono), MP_ROM_PTR(&mp3dec_set_mono_obj) }, // New
    { MP_ROM_QSTR(MP_QSTR_get_sample_rate), MP_ROM_PTR(&mp3dec_get_sample_rate_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_bitrate), MP_ROM_PTR(&mp3dec_get_bitrate_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_channels), MP_ROM_PTR(&mp3dec_get_channels_obj) },
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
