idf_component_register(SRCS "heatshrink_encoder.c" "heatshrink_decoder.c"
                        INCLUDE_DIRS "include")

# heatshrink_encoder.c:237:24: warning: this statement may fall through [-Wimplicit-fallthrough=]
#   237 |             hse->state = st_flush_bit_buffer(hse, &oi);
#       |             ~~~~~~~~~~~^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
# heatshrink_encoder.c:238:9: note: here
#   238 |         case HSES_DONE:
#       |         ^~~~

set_source_files_properties("heatshrink_encoder.c" PROPERTIES COMPILE_OPTIONS "-Wno-implicit-fallthrough")
