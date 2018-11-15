#include "priv-view.hpp"
#include "core.hpp"
#include "output.hpp"
#include "opengl.hpp"
#include "workspace-manager.hpp"
#include "compositor-view.hpp"
#include "debug.hpp"

#include <glm/gtc/matrix_transform.hpp>

extern "C"
{
#define static
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_matrix.h>
#undef static
}

wayfire_compositor_view_t::wayfire_compositor_view_t()
{
    role = WF_VIEW_ROLE_TOPLEVEL;
}

wf_point wayfire_compositor_view_t::get_output_position()
{
    return {geometry.x, geometry.y};
}

wf_geometry wayfire_compositor_view_t::get_output_geometry()
{
    return geometry;
}

wf_geometry wayfire_compositor_view_t::get_wm_geometry()
{
    return geometry;
}

void wayfire_compositor_view_t::set_geometry(wf_geometry g)
{
    damage();
    geometry = g;
    damage();
}

void wayfire_compositor_view_t::map()
{
    if (_is_mapped)
        return;
    _is_mapped = true;

    output->attach_view(self());
    emit_view_map(self());
    emit_map_state_change(this);
}

void wayfire_compositor_view_t::unmap()
{
    if (!_is_mapped)
        return;

    if (output)
        emit_view_unmap(self());

    _is_mapped = false;

    if (output)
        emit_map_state_change(this);
}

void wayfire_compositor_view_t::close()
{
    unmap();
    destroy();
}

void wayfire_compositor_view_t::render_fb(pixman_region32_t *damage, wf_framebuffer fb)
{
    /* We only want to circumvent wayfire_surface_t::render_fb,
     * because it relies on the surface having a buffer
     *
     * If there is a transform though, we're fine with what wayfire_view_t provides */
    if (has_transformer())
        return wayfire_view_t::render_fb(damage, fb);

    GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb.fb));
    auto obox = get_output_geometry();
    render_pixman(wlr_fb_attribs{fb}, obox.x - fb.geometry.x, obox.y - fb.geometry.y, damage);
}

void wayfire_mirror_view_t::_wlr_render_box(const wlr_fb_attribs& fb,
        int x, int y, const wlr_box& scissor)
{
    wlr_renderer_begin(core->renderer, fb.width, fb.height);
    GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, fb.fb));
    auto sbox = scissor; wlr_renderer_scissor(core->renderer, &sbox);

    auto ortho = glm::ortho(0.0f, 1.0f * fb.width, 1.0f * fb.height, 0.0f);

    gl_geometry render_geometry;
    render_geometry.x1 = x;
    render_geometry.y1 = y;
    render_geometry.x2 = x + geometry.width;
    render_geometry.y2 = y + geometry.height;

    OpenGL::render_transformed_texture(offscreen_buffer.tex, render_geometry, {}, ortho);
    wlr_renderer_end(core->renderer);
}

void wayfire_mirror_view_t::_render_pixman(const wlr_fb_attribs& fb, int x, int y, pixman_region32_t *damage)
{
    take_snapshot();
    wayfire_compositor_view_t::_render_pixman(fb, x, y, damage);
}

wayfire_mirror_view_t::wayfire_mirror_view_t(wayfire_view original_view)
{
    this->original_view = original_view;

    base_view_unmapped = [=] (signal_data*) {
        unmap();
        destroy();
    };

    original_view->connect_signal("unmap", &base_view_unmapped);

    base_view_damaged = [=] (signal_data* ) {
        damage();
    };

    original_view->connect_signal("damaged-region", &base_view_damaged);
}

bool wayfire_mirror_view_t::can_take_snapshot()
{
    return original_view.get();
}

void wayfire_mirror_view_t::take_snapshot()
{
    if (!can_take_snapshot())
        return;

    auto obox = original_view->get_bounding_box();
    geometry.width = obox.width;
    geometry.height = obox.height;

    float scale = output ? output->handle->scale : 1;
    int scaled_width = scale * obox.width;
    int scaled_height = scale * obox.height;

    if (offscreen_buffer.fb_width != scaled_width ||
        offscreen_buffer.fb_height != scaled_height)
    {
        offscreen_buffer.fini();
    }

    if (!offscreen_buffer.valid())
        offscreen_buffer.init(scaled_width, scaled_height);

    auto buffer_geometry = get_untransformed_bounding_box();
    offscreen_buffer.output_x = buffer_geometry.x;
    offscreen_buffer.output_y = buffer_geometry.y;
    offscreen_buffer.fb_scale = scale;

    GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, offscreen_buffer.fbo));
    wlr_renderer_begin(core->renderer, offscreen_buffer.fb_width, offscreen_buffer.fb_height);
    wlr_renderer_scissor(core->renderer, NULL);
    float clear_color[] = {0, 0, 0, 0};
    wlr_renderer_clear(core->renderer, clear_color);
    wlr_renderer_end(core->renderer);

    wf_framebuffer fb;
    fb.fb = offscreen_buffer.fbo;
    fb.tex = offscreen_buffer.tex;

    fb.geometry = obox;
    fb.viewport_width = scaled_width;
    fb.viewport_height = scaled_height;

    original_view->render_fb(NULL, fb);
}

wf_point wayfire_mirror_view_t::get_output_position()
{
    return wayfire_compositor_view_t::get_output_position();
}

wf_geometry wayfire_mirror_view_t::get_output_geometry()
{
    if (!original_view)
        return wayfire_compositor_view_t::get_output_geometry();

    auto bbox = original_view->get_bounding_box();
    geometry.width = bbox.width;
    geometry.height = bbox.height;

    return geometry;
}

wf_geometry wayfire_mirror_view_t::get_wm_geometry()
{
    if (!original_view)
        return wayfire_compositor_view_t::get_wm_geometry();

    auto bbox = original_view->get_bounding_box();
    geometry.width = bbox.width;
    geometry.height = bbox.height;

    return geometry;
}

void wayfire_mirror_view_t::unset_original_view()
{
    original_view->disconnect_signal("damaged-region", &base_view_damaged);
    original_view->disconnect_signal("unmap", &base_view_unmapped);

    original_view = nullptr;
}

void wayfire_mirror_view_t::unmap()
{
    unset_original_view();
    wayfire_compositor_view_t::unmap();
}

void wayfire_color_rect_view_t::_render_rect(float projection[9],
    int x, int y, int w, int h, const wf_color& color)
{
    wlr_box render_geometry_unscaled {x, y, w, h};
    wlr_box render_geometry = output_transform_box(output,
        render_geometry_unscaled);

    float matrix[9];
    wlr_matrix_project_box(matrix, &render_geometry, WL_OUTPUT_TRANSFORM_NORMAL, 0, projection);

    float col[4] = {color.r * color.a, color.g * color.a, color.b * color.a, color.a};
    wlr_render_quad_with_matrix(core->renderer, col, matrix);
}

void wayfire_color_rect_view_t::_wlr_render_box(const wlr_fb_attribs& fb, int x, int y, const wlr_box& scissor)
{
    float projection[9];
    wlr_matrix_projection(projection, fb.width, fb.height, fb.transform);

    wlr_renderer_begin(core->renderer, fb.width, fb.height);
    auto sbox = scissor; wlr_renderer_scissor(core->renderer, &sbox);

    /* Draw the border, making sure border parts don't overlap, otherwise
     * we will get wrong corners if border has alpha != 1.0 */
    // top
    _render_rect(projection, x, y, geometry.width, border,
        _border_color);
    // bottom
    _render_rect(projection, x, y + geometry.height - border,
        geometry.width, border, _border_color);
    // left
    _render_rect(projection, x, y + border, border, geometry.height - 2 * border,
        _border_color);
    // right
    _render_rect(projection, x + geometry.width - border, y + border, border,
        geometry.height - 2 * border, _border_color);

    /* Draw the inside of the rect */
    _render_rect(projection, x + border, y + border,
        geometry.width - 2 * border, geometry.height - 2 * border,
        _color);

    wlr_renderer_end(core->renderer);
}

wayfire_color_rect_view_t::wayfire_color_rect_view_t()
{
    set_color({0, 0, 0, 0.5});
    set_border_color({1, 1, 1, 1});
    set_border(20);
}

void wayfire_color_rect_view_t::set_color(wf_color color)
{
    _color = color;
    damage();
}

void wayfire_color_rect_view_t::set_border_color(wf_color border)
{
    _border_color = border;
    damage();
}

void wayfire_color_rect_view_t::set_border(int width)
{
    border = width;
    damage();
}
