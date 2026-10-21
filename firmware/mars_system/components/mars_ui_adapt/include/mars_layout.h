#pragma once
/* Portable responsive geometry. No board, display, heap or OS dependency.
 * density_q8=256 means one physical pixel per layout unit. */
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {int x,y,width,height;} mars_layout_rect_t;
typedef enum {MARS_LAYOUT_COMPACT,MARS_LAYOUT_REGULAR,MARS_LAYOUT_EXPANDED} mars_layout_tier_t;
typedef struct {
    int width,height,inset,density_q8;
    bool round,footer;
} mars_layout_spec_t;
typedef struct {
    mars_layout_rect_t frame,header,body,footer;
    mars_layout_tier_t tier;
    int density_q8,gap,touch_min,body_font,title_font;
} mars_layout_plan_t;
typedef struct {
    mars_layout_rect_t area;
    int columns,rows,cell_width,cell_height,gap,visible_count,page_count;
} mars_layout_grid_t;
bool mars_layout_solve(mars_layout_spec_t,mars_layout_plan_t *);
/* Oversized lists paginate instead of shrinking controls below their minimum. */
bool mars_layout_grid(const mars_layout_plan_t *,int min_width_units,int min_height_units,
                      int max_columns,int item_count,mars_layout_grid_t *);
bool mars_layout_cell(const mars_layout_grid_t *,int visible_index,mars_layout_rect_t *);
bool mars_layout_hit(mars_layout_rect_t,int x,int y);

#ifdef __cplusplus
}
#endif
