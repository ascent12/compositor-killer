#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <linux/sync_file.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include "xdg-shell-protocol.h"

struct wl_state {
	struct wl_compositor *wl_compositor;
	struct xdg_wm_base *xdg_wm_base;

	bool close;
	uint32_t serial;
	int32_t width;
	int32_t height;

	struct wl_callback *frame;
};

static const GLchar *vert_src =
"precision highp float;\n"
"attribute vec2 in_pos;\n"
"varying vec2 v_pos;\n"
"void main() {\n"
"	v_pos = in_pos;\n"
"	gl_Position = vec4(in_pos, 0.0, 1.0);\n"
"}\n";

static const GLchar *frag_src =
"precision highp float;\n"
"uniform float offset;\n"
"uniform int iter;\n"
"varying vec2 v_pos;\n"
"vec3 hue2rgb(float h) {\n"
"	vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);\n"
"	vec3 p = abs(fract(vec3(h, h, h) + K.xyz) * 6.0 - K.www);\n"
"	return mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), 1.0);\n"
"}\n"
"void main() {\n"
"	float x0 = 1.5 * v_pos.x - 0.5;\n"
"	float y0 = 1.5 * v_pos.y;\n"
"	float x = 0.0;\n"
"	float y = 0.0;\n"
"	int i = 0;\n"
"	while (x * x + y * y <= 4.0 && i < iter) {\n"
"		float xtemp = x * x - y * y + x0;\n"
"		y = 2.0 * x * y + y0;\n"
"		x = xtemp;\n"
"		++i;\n"
"	}\n"
"	if (i == iter) {\n"
"		gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
"	} else {\n"
"		float frac = float(iter) / float(i);\n"
"		vec3 rgb = hue2rgb(log(frac) / 15.0 + offset);\n"
"		gl_FragColor = vec4(rgb, 1.0);\n"
"	}\n"
"}\n";

static void xdg_ping(void *data, struct xdg_wm_base *shell, uint32_t serial)
{
	xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener shell_listener = {
	.ping = xdg_ping,
};

static void global_add(void *data, struct wl_registry *reg, uint32_t name,
		const char *iface, uint32_t version)
{
	struct wl_state *wl_state = data;

	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		wl_state->wl_compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 1);

	} else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		wl_state->xdg_wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wl_state->xdg_wm_base, &shell_listener, NULL);
	}
}

static void global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	/* Don't care */
}

static const struct wl_registry_listener registry_listener = {
	.global = global_add,
	.global_remove = global_remove,
};

static void xdg_base_configure(void *data, struct xdg_surface *surf, uint32_t serial)
{
	struct wl_state *wl_state = data;
	wl_state->serial = serial;
}

static const struct xdg_surface_listener xdg_base_listener = {
	.configure = xdg_base_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *top,
		int32_t width, int32_t height, struct wl_array *states)
{
	struct wl_state *wl_state = data;
	wl_state->width = width;
	wl_state->height = height;
}

static void toplevel_close(void *data, struct xdg_toplevel *top)
{
	struct wl_state *wl_state = data;
	wl_state->close = true;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static bool has_ext(const char *exts, const char *ext)
{
	while (*exts) {
		const char *end = strchr(exts, ' ');
		size_t len = end ? (size_t)(end - exts) : strlen(exts);

		if (strncmp(exts, ext, len) == 0)
			return true;
		exts += len + !!end;
	}

	return false;
}

static GLuint compile_shader(const GLchar *src, GLenum type, const char *tag)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);

	GLint status = GL_TRUE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status) {
		char log[1000];
		GLsizei len;
		glGetShaderInfoLog(shader, sizeof log, &len, log);
		fprintf(stderr, "%s: %s\n", tag, log);

		return 0;
	}

	return shader;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	struct wl_state *wl_state = data;
	wl_callback_destroy(wl_state->frame);
	wl_state->frame = NULL;
}

static const struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

static uint64_t fence_timestamp(int fd)
{
	struct sync_file_info file = {0};

	if (ioctl(fd, SYNC_IOC_FILE_INFO, &file) == -1) {
		perror("SYNC_IOC_FILE_INFO");
		return 0;
	}

	struct sync_fence_info fences[file.num_fences];
	file.sync_fence_info = (__u64)(uintptr_t)fences;

	if (ioctl(fd, SYNC_IOC_FILE_INFO, &file) == -1) {
		perror("SYNC_IOC_FILE_INFO");
		return 0;
	}

	uint64_t latest = 0;
	for (__u32 i = 0; i < file.num_fences; ++i) {
		if (fences[i].timestamp_ns > latest)
			latest = fences[i].timestamp_ns;
	}

	return latest;
}

int main(int argc, char *argv[])
{
	int iter = 1000;
	bool fixed_size = false;
	int fixed_width = 0;
	int fixed_height = 0;
	int max_frames = INT_MAX;
	bool unsynchronized = false;

	/* Command line parsing */
	{
		int opt;
		while ((opt = getopt(argc, argv, "i:f:l:u")) != -1) {
			switch (opt) {
			case 'i':
				iter = atoi(optarg);
				break;
			case 'f':
				fixed_size = true;
				if (sscanf(optarg, "%dx%d", &fixed_width, &fixed_height) != 2)
					return 1;
				break;
			case 'l':
				max_frames = atoi(optarg);
				break;
			case 'u':
				unsynchronized = true;
				break;
			default:
				return 1;
			}
		}
	}

	/* Wayland */

	struct wl_display *wl_display;
	struct wl_state wl_state = {0};

	wl_display = wl_display_connect(NULL);
	if (!wl_display) {
		perror("wl_display_connect");
		return 1;
	}
	
	/* Fetching Wayland globals */
	{
		struct wl_registry *wl_reg;
		wl_reg = wl_display_get_registry(wl_display);
		wl_registry_add_listener(wl_reg, &registry_listener, &wl_state);
		wl_display_roundtrip(wl_display);
		wl_registry_destroy(wl_reg);

		if (!wl_state.wl_compositor) {
			fprintf(stderr, "wl_compositor: %s\n", strerror(EPROTONOSUPPORT));
			return 1;
		}
		if (!wl_state.xdg_wm_base) {
			fprintf(stderr, "xdg_wm_base: %s\n", strerror(EPROTONOSUPPORT));
			return 1;
		}
	}

	/* EGL */

	EGLDisplay egl_display;
	EGLConfig egl_config;
	EGLContext egl_context;
	bool egl_has_fences = false;
	PFNEGLCREATESYNCKHRPROC egl_create_sync = NULL;
	PFNEGLDESTROYSYNCKHRPROC egl_destroy_sync = NULL;
	PFNEGLDUPNATIVEFENCEFDANDROIDPROC egl_dup_fence = NULL;

	/* Querying EGL client extensions */
	{
		const char *exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
		if (!exts) {
			fprintf(stderr, "EGL_EXT_client_extensions: %s\n",
				strerror(ENOTSUP));
			return 1;
		}

		if (!has_ext(exts, "EGL_EXT_platform_wayland")) {
			fprintf(stderr, "EGL_EXT_platform_wayland: %s\n",
				strerror(ENOTSUP));
			return 1;
		}
	}

	/* Initializing EGL */
	{
		PFNEGLGETPLATFORMDISPLAYEXTPROC egl_get_display;
		egl_get_display = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");

		egl_display = egl_get_display(EGL_PLATFORM_WAYLAND_EXT, wl_display, NULL);
		if (!egl_display) {
			fprintf(stderr, "eglGetPlatformDisplayEXT: 0x%x\n", eglGetError());
			return 1;
		}

		if (!eglInitialize(egl_display, NULL, NULL)) {
			fprintf(stderr, "eglInitialize: 0x%x\n", eglGetError());
			return 1;
		}
	}

	/* Querying EGL display extensions */
	{
		const char *exts = eglQueryString(egl_display, EGL_EXTENSIONS);

		if (has_ext(exts, "EGL_ANDROID_native_fence_sync")) {
			egl_has_fences = true;
			egl_create_sync = (void *)eglGetProcAddress("eglCreateSyncKHR");
			egl_destroy_sync = (void *)eglGetProcAddress("eglDestroySyncKHR");
			egl_dup_fence = (void *)eglGetProcAddress("eglDupNativeFenceFDANDROID");
		}
	}

	/* Choosing an EGL config */
	{
		static const EGLint conf_attribs[] = {
			EGL_RED_SIZE, 8,
			EGL_GREEN_SIZE, 8,
			EGL_BLUE_SIZE, 8,
			EGL_ALPHA_SIZE, 0,
			EGL_NONE
		};
		EGLint num_confs;

		if (!eglChooseConfig(egl_display, conf_attribs, &egl_config, 1, &num_confs) || !num_confs) {
			fprintf(stderr, "eglChooseConfig: 0x%x\n", eglGetError());
			return 1;
		}
	}

	/* Creating an EGL context */
	{
		static const EGLint context_attribs[] = {
			EGL_CONTEXT_CLIENT_VERSION, 2,
			EGL_NONE
		};

		egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attribs);
		if (!egl_context) {
			fprintf(stderr, "eglCreateContext: 0x%x\n", eglGetError());
			return 1;
		}
	}

	/* Surface */

	struct wl_surface *surface_wl;
	struct xdg_surface *surface_xdg_base;
	struct xdg_toplevel *surface_xdg_toplevel;
	struct wl_egl_window *surface_egl_native;
	EGLSurface surface_egl;

	/* Creating Wayland surface */
	{
		surface_wl           = wl_compositor_create_surface(wl_state.wl_compositor);
		surface_xdg_base     = xdg_wm_base_get_xdg_surface(wl_state.xdg_wm_base, surface_wl);
		surface_xdg_toplevel = xdg_surface_get_toplevel(surface_xdg_base);

		xdg_surface_add_listener(surface_xdg_base, &xdg_base_listener, &wl_state);
		xdg_toplevel_add_listener(surface_xdg_toplevel, &toplevel_listener, &wl_state);

		xdg_toplevel_set_title(surface_xdg_toplevel, "compositor-killer");
		if (fixed_size) {
			xdg_toplevel_set_max_size(surface_xdg_toplevel, fixed_width, fixed_height);
			xdg_toplevel_set_min_size(surface_xdg_toplevel, fixed_width, fixed_height);
		}

		wl_surface_commit(surface_wl);
		wl_display_roundtrip(wl_display);
	}

	/* Creating EGL surface */
	{
		PFNEGLCREATEPLATFORMWINDOWSURFACEPROC egl_create_surface;
		egl_create_surface = (void *)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");

		if (fixed_size) {
			wl_state.width = fixed_width;
			wl_state.height = fixed_height;
		}

		if (wl_state.width == 0)
			wl_state.width = 500;
		if (wl_state.height == 0)
			wl_state.height = 500;

		surface_egl_native =
			wl_egl_window_create(surface_wl, wl_state.width, wl_state.height);
		if (!surface_egl_native) {
			perror("wl_egl_window_create");
			return 1;
		}

		surface_egl = egl_create_surface(egl_display, egl_config, surface_egl_native, NULL);
		if (!surface_egl) {
			fprintf(stderr, "eglCreatePlatformWindowSurfaceEXT: 0x%x",
				eglGetError());
			return 1;
		}
	}

	/* Making EGL surface current */
	eglMakeCurrent(egl_display, surface_egl, surface_egl, egl_context);
	eglSwapInterval(egl_display, 0);

	/* OpenGL */
	GLuint gl_program;
	GLuint gl_uniform_offset;

	/* Compile GL shaders */
	{
		GLuint vert;
		GLuint frag;
		GLint status = GL_TRUE;

		vert = compile_shader(vert_src, GL_VERTEX_SHADER, "vert_src");
		frag = compile_shader(frag_src, GL_FRAGMENT_SHADER, "frag_src");

		gl_program = glCreateProgram();
		glAttachShader(gl_program, vert);
		glAttachShader(gl_program, frag);
		glBindAttribLocation(gl_program, 0, "in_pos");
		glLinkProgram(gl_program);

		glGetProgramiv(gl_program, GL_LINK_STATUS, &status);
		if (!status) {
			char log[1000];
			GLsizei len;
			glGetProgramInfoLog(gl_program, sizeof log, &len, log);
			printf("shader: %s\n", log);
			return 1;
		}
		glDeleteShader(vert);
		glDeleteShader(frag);
	}

	gl_uniform_offset = glGetUniformLocation(gl_program, "offset");

	/* Bind all GL state now, because it will never change */
	{
		static const GLfloat verts[] = {
			-1.0f, -1.0f,
			-1.0f, 1.0f,
			1.0f, 1.0f,
			1.0f, -1.0f,
		};
		GLuint attr_in_pos = glGetAttribLocation(gl_program, "in_pos");
		GLuint uniform_iter = glGetUniformLocation(gl_program, "iter");

		glUseProgram(gl_program);

		glEnableVertexAttribArray(attr_in_pos);
		glVertexAttribPointer(attr_in_pos, 2, GL_FLOAT, GL_FALSE, 0, verts);

		glUniform1i(uniform_iter, iter);
	}

	/* Main loop */
	size_t len = 1;
	size_t cap = 10;
	struct pollfd *fds;
	struct { int frame_num; uint64_t start_ns; } *fences;

	fds = calloc(cap, sizeof *fds);
	fences = calloc(cap, sizeof *fences);

	if (!fds || !fences)
		return 1;

	// fds[0] is always reserved for wl_display
	fds[0].fd = wl_display_get_fd(wl_display);
	fds[0].events = POLLIN | POLLOUT;

	int frame_num = 0;
	float color_offset = 0.0f;

	while (!wl_state.close && frame_num < max_frames) {
		int ret;

		/* Render */

		if (unsynchronized || !wl_state.frame) {
			EGLSyncKHR sync;
			uint64_t start_ns = 0;

			if (!unsynchronized) {
				wl_state.frame = wl_surface_frame(surface_wl);
				wl_callback_add_listener(wl_state.frame, &frame_listener, &wl_state);
			}

			/* Resize window */

			if (wl_state.serial) {
				if (fixed_size) {
					wl_state.width = fixed_width;
					wl_state.height = fixed_height;
				}

				if (wl_state.width == 0)
					wl_state.width = 500;
				if (wl_state.height == 0)
					wl_state.height = 500;

				wl_egl_window_resize(surface_egl_native, wl_state.width, wl_state.height, 0, 0);

				xdg_surface_ack_configure(surface_xdg_base, wl_state.serial);
				wl_state.serial = 0;
			}

			glViewport(0, 0, wl_state.width, wl_state.height);

			glUniform1f(gl_uniform_offset, color_offset);
			color_offset += 0.01;

			glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

			if (egl_has_fences) {
				struct timespec ts = {0};
				sync = egl_create_sync(egl_display, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);

				/*
				 * TODO: Check if MONOTONIC is guranteed to be the right time domain.
				 *
				 * Sampling the clock from userspace might not be the most accurate way
				 * to do this, but it's good enough for our purposes.
				 */
				clock_gettime(CLOCK_MONOTONIC, &ts);
				start_ns = ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
			}

			eglSwapBuffers(egl_display, surface_egl);

			if (egl_has_fences) {
				if (len == cap) {
					cap *= 2;
					fds = realloc(fds, sizeof *fds * cap);
					fences = realloc(fences, sizeof *fences * cap);

					if (!fds || !fences)
						return 1;
				}

				fds[len].fd = egl_dup_fence(egl_display, sync);
				fds[len].events = POLLIN;
				//fds[len].revents = 0;
				fences[len].frame_num = frame_num;
				fences[len].start_ns = start_ns;

				egl_destroy_sync(egl_display, sync);
				++len;
			}

			++frame_num;
		}

		while (wl_display_prepare_read(wl_display) != 0 && errno == EAGAIN)
			wl_display_dispatch_pending(wl_display);

		errno = 0;
		do {
			ret = wl_display_flush(wl_display);
		} while (ret > 0);

		if (ret == -1) {
			if (errno == EAGAIN) {
				fds[0].events |= POLLOUT;
			} else {
				wl_display_cancel_read(wl_display);
				break;
			}
		} else {
			/* Don't read POLLOUT if we don't need to. It wakes up poll too often. */
			fds[0].events &= ~POLLOUT;
		}

		ret = poll(fds, len, unsynchronized ? 0 : -1);
		if (ret == -1 && errno != EINTR) {
			perror("poll");
			wl_display_cancel_read(wl_display);
			break;
		}

		if (fds[0].revents & (POLLERR | POLLHUP)) {
			wl_display_cancel_read(wl_display);
			break;
		}

		wl_display_read_events(wl_display);
		wl_display_dispatch_pending(wl_display);

		/* Read out rendering times where complete */

		for (size_t i = 1; i < len;) {
			uint64_t end_ns;

			if (!(fds[i].revents & POLLIN)) {
				++i;
				continue;
			}

			end_ns = fence_timestamp(fds[i].fd);
			close(fds[i].fd);

			printf("Frame %d: %f ms\n", fences[i].frame_num,
				(double)(end_ns - fences[i].start_ns) * 1e-6);

			for (size_t j = i; j < len - 1; ++j) {
				fds[j] = fds[j + 1];
				fences[j] = fences[j + 1];
			}
			--len;
		}
	}

	if (wl_state.frame)
		wl_callback_destroy(wl_state.frame);

	for (size_t i = 1; i < len; ++i)
		close(fds[i].fd);
	free(fds);
	free(fences);

	glDeleteProgram(gl_program);

	eglDestroySurface(egl_display, surface_egl);
	wl_egl_window_destroy(surface_egl_native);

	xdg_toplevel_destroy(surface_xdg_toplevel);
	xdg_surface_destroy(surface_xdg_base);
	wl_surface_destroy(surface_wl);

	eglDestroyContext(egl_display, egl_context);
	eglTerminate(egl_display);

	eglMakeCurrent(NULL, NULL, NULL, NULL);
	eglReleaseThread();

	xdg_wm_base_destroy(wl_state.xdg_wm_base);
	wl_compositor_destroy(wl_state.wl_compositor);

	wl_display_disconnect(wl_display);
}
