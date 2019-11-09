#include <errno.h>
#include <inttypes.h>
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

struct state {
	struct wl_display *wl;
	struct wl_compositor *comp;
	struct xdg_wm_base *shell;

	EGLDisplay egl;
	EGLContext context;
	PFNEGLCREATESYNCKHRPROC create_sync;
	PFNEGLDESTROYSYNCKHRPROC destroy_sync;
	PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence;

	struct wl_surface *surf;
	struct xdg_surface *xdg;
	struct xdg_toplevel *toplevel;
	struct wl_egl_window *native;
	EGLSurface egl_surf;

	GLint color_loc;

	struct wl_callback *frame;

	bool running;
	struct {
		uint32_t serial;
		int32_t width;
		int32_t height;
	} conf;

	uint64_t submit_time_ns;
	int fence;
};

static const GLchar *vert_src =
"precision highp float;\n"
"attribute vec2 in_pos;\n"
"void main() {\n"
"	gl_Position = vec4(in_pos, 0.0, 1.0);\n"
"}\n";

static const GLchar *frag_src =
"precision highp float;\n"
"uniform vec4 color;\n"
"void main() {\n"
"	gl_FragColor = color;\n"
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
	struct state *st = data;

	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		st->comp = wl_registry_bind(reg, name, &wl_compositor_interface, 1);
	} else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		st->shell = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(st->shell, &shell_listener, NULL);
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

static void xdg_configure(void *data, struct xdg_surface *surf, uint32_t serial)
{
	struct state *st = data;
	st->conf.serial = serial;
}

static const struct xdg_surface_listener xdg_listener = {
	.configure = xdg_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *top,
		int32_t width, int32_t height, struct wl_array *states)
{
	struct state *st = data;
	st->conf.width = width;
	st->conf.height = height;
}

static void toplevel_close(void *data, struct xdg_toplevel *top)
{
	struct state *st = data;
	st->running = false;
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

		exit(1);
	}

	return shader;
}

static const struct wl_callback_listener frame_listener;

static void draw(struct state *st)
{
	struct timespec ts;
	EGLSyncKHR sync;

	st->frame = wl_surface_frame(st->surf);
	wl_callback_add_listener(st->frame, &frame_listener, st);

	if (st->conf.serial) {
		if (!st->conf.width)
			st->conf.width = 500;
		if (!st->conf.height)
			st->conf.height = 500;

		wl_egl_window_resize(st->native, st->conf.width, st->conf.height, 0, 0);

		xdg_surface_ack_configure(st->xdg, st->conf.serial);
		st->conf.serial = 0;
	}

	glViewport(0, 0, st->conf.width, st->conf.height);

	//glUniform4f(st->color_loc, 0.0, 0.2, 0.0, 0.2);

	glClearColor(0.5, 0.0, 0.0, 0.5);
	glClear(GL_COLOR_BUFFER_BIT);

	//glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	sync = st->create_sync(st->egl, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);

	/*
	 * TODO: Check if MONOTONIC is guranteed to be the right time domain.
	 *
	 * Sampling the clock from userspace might not be the most accurate way
	 * to do this, but it's good enough for our purposes.
	 */
	clock_gettime(CLOCK_MONOTONIC, &ts);
	st->submit_time_ns = ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;

	eglSwapBuffers(st->egl, st->egl_surf);

	st->fence = st->dup_fence(st->egl, sync);
	st->destroy_sync(st->egl, sync);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	struct state *st = data;
	wl_callback_destroy(st->frame);
	st->frame = NULL;

	draw(st);
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
	struct state st = {
		.running = true,
	};
	struct wl_registry *reg;
	const char *client_exts;
	PFNEGLGETPLATFORMDISPLAYPROC get_display;
	PFNEGLCREATEPLATFORMWINDOWSURFACEPROC create_surface;
	static const EGLint conf_attribs[] = {
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 1,
		EGL_NONE
	};
	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	EGLConfig conf;
	EGLint num_confs;
	GLuint vert;
	GLuint frag;
	GLuint prog;
	GLint status = GL_TRUE;
	static const GLfloat verts[] = {
		-1.0f, -1.0f,
		-1.0f, 1.0f,
		1.0f, 1.0f,
		1.0f, -1.0f,
	};

	st.wl = wl_display_connect(NULL);
	if (!st.wl) {
		perror("wl_display_connect");
		return 1;
	}

	/* Fetching Wayland globals */

	reg = wl_display_get_registry(st.wl);
	wl_registry_add_listener(reg, &registry_listener, &st);
	wl_display_roundtrip(st.wl);
	wl_registry_destroy(reg);

	if (!st.comp) {
		fprintf(stderr, "wl_compositor: %s\n", strerror(EPROTONOSUPPORT));
		return 1;
	}
	if (!st.shell) {
		fprintf(stderr, "xdg_wm_base: %s\n", strerror(EPROTONOSUPPORT));
		return 1;
	}

	/* Initializing EGL */

	client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	if (!client_exts) {
		fprintf(stderr, "EGL_EXT_client_extensions: %s\n",
			strerror(ENOTSUP));
		return 1;
	}

	if (!has_ext(client_exts, "EGL_EXT_platform_wayland")) {
		fprintf(stderr, "EGL_EXT_platform_wayland: %s\n",
			strerror(ENOTSUP));
		return 1;
	}

	get_display = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
	create_surface = (void *)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");

	st.egl = get_display(EGL_PLATFORM_WAYLAND_EXT, st.wl, NULL);
	if (!st.egl) {
		fprintf(stderr, "eglGetPlatformDisplayEXT: 0x%x\n", eglGetError());
		return 1;
	}

	if (!eglInitialize(st.egl, NULL, NULL)) {
		fprintf(stderr, "eglInitialize: 0x%x\n", eglGetError());
		return 1;
	}

	st.create_sync = (void *)eglGetProcAddress("eglCreateSyncKHR");
	st.destroy_sync = (void *)eglGetProcAddress("eglDestroySyncKHR");
	st.dup_fence = (void *)eglGetProcAddress("eglDupNativeFenceFDANDROID");

	if (!eglChooseConfig(st.egl, conf_attribs, &conf, 1, &num_confs) || !num_confs) {
		fprintf(stderr, "eglChooseConfig: 0x%x\n", eglGetError());
		return 1;
	}

	st.context = eglCreateContext(st.egl, conf, EGL_NO_CONTEXT, context_attribs);
	if (!st.context) {
		fprintf(stderr, "eglCreateContext: 0x%x\n", eglGetError());
		return 1;
	}

	/* Creating Wayland surface */

	st.surf = wl_compositor_create_surface(st.comp);
	st.xdg = xdg_wm_base_get_xdg_surface(st.shell, st.surf);
	st.toplevel = xdg_surface_get_toplevel(st.xdg);

	xdg_surface_add_listener(st.xdg, &xdg_listener, &st);
	xdg_toplevel_add_listener(st.toplevel, &toplevel_listener, &st);

	wl_surface_commit(st.surf);
	wl_display_roundtrip(st.wl);

	if (st.conf.width == 0)
		st.conf.width = 500;
	if (st.conf.height == 0)
		st.conf.height = 500;

	st.native = wl_egl_window_create(st.surf,
		st.conf.width, st.conf.height);
	if (!st.native) {
		perror("wl_egl_window_create");
		return 1;
	}

	st.egl_surf = create_surface(st.egl, conf, st.native, NULL);
	if (!st.egl_surf) {
		fprintf(stderr, "eglCreatePlatformWindowSurfaceEXT: 0x%x",
			eglGetError());
		return 1;
	}

	eglMakeCurrent(st.egl, st.egl_surf, st.egl_surf, st.context);
	eglSwapInterval(st.egl, 0);

	/* Compile GL shaders */

	vert = compile_shader(vert_src, GL_VERTEX_SHADER, "vert_src");
	frag = compile_shader(frag_src, GL_FRAGMENT_SHADER, "frag_src");

	prog = glCreateProgram();
	glAttachShader(prog, vert);
	glAttachShader(prog, frag);
	glBindAttribLocation(prog, 0, "in_pos");
	glLinkProgram(prog);

	glGetProgramiv(prog, GL_LINK_STATUS, &status);
	if (!status) {
		char log[1000];
		GLsizei len;
		glGetProgramInfoLog(prog, sizeof log, &len, log);
		printf("shader: %s\n", log);
		return 1;
	}
	glDeleteShader(vert);
	glDeleteShader(frag);

	st.color_loc = glGetUniformLocation(prog, "color");

	/* Bind all GL state now, because it will never change */

	glUseProgram(prog);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);

	/* Main loop */

	draw(&st);

	struct pollfd fds[2] = {
		{ .fd = wl_display_get_fd(st.wl), .events = POLLIN | POLLOUT },
		{ .fd = st.fence, .events = POLLIN },
	};
	st.fence = -1;

	while (st.running) {
		int ret = poll(fds, 2, 0);
		if (ret == -1) {
			perror("poll");
			break;
		}

		if (fds[1].fd != -1 && (fds[1].revents & POLLIN)) {
			uint64_t time_ns = fence_timestamp(fds[1].fd);
			close(fds[1].fd);
			fds[1].fd = -1;
			fds[1].events = 0;

			printf("Took %f ms to render (%lu - %lu)\n",
				(double)(time_ns - st.submit_time_ns) * 1e-6,
				st.submit_time_ns, time_ns);
			st.submit_time_ns = 0;
		}

		if (fds[0].revents & (POLLERR | POLLHUP)) {
			break;
		} else if (fds[0].revents & POLLIN) {
			wl_display_dispatch(st.wl);
		} else if (fds[0].revents & POLLOUT) {
			wl_display_flush(st.wl);
		}

		if (st.fence != -1) {
			fds[1].fd = st.fence;
			fds[1].events = POLLIN;

			st.fence = -1;
		}
	}

	if (st.running)
		perror("wl_display_dispatch");

	wl_compositor_destroy(st.comp);
	xdg_wm_base_destroy(st.shell);
	wl_display_disconnect(st.wl);
}
