/*
 * gui_smoke.c - Kiem tra GTK3/WSLg truoc khi lam GUI nghiep vu.
 * Chay: ./bin/ltm-gui-smoke (xem cua so); --check (realize cua so roi thoat).
 * Day la smoke test moi truong, chua phai GUI chat da hoan thanh.
 */
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

/*
 * main
 * Y nghia : Mo GTK display, tao native window; che do check khong map cua so.
 * Dau vao : argc/argv - tuy chon --check; khong co thi xem cua so truc tiep.
 * Dau ra  : 0 neu tao duoc window, 1 khi thieu display, 2 tham so sai.
 */
int main(int argc, char *argv[])
{
    int check = argc == 2 && strcmp(argv[1], "--check") == 0;
    if (argc > 1 && !check) { fprintf(stderr, "Usage: %s [--check]\n", argv[0]); return 2; }
    if (!gtk_init_check(NULL, NULL)) { fprintf(stderr, "GTK display unavailable\n"); return 1; }
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Chat LTM - Giai doan 1");
    gtk_window_set_default_size(GTK_WINDOW(window), 480, 200);
    GtkWidget *label = gtk_label_new("GTK3/WSLg san sang\nTCP echo: dung bin/ltm-client");
    gtk_container_add(GTK_CONTAINER(window), label);
    gtk_widget_realize(window);
    if (gtk_widget_get_window(window) == NULL) { gtk_widget_destroy(window); return 1; }
    if (check) {
        printf("GTK smoke: PASS (%u.%u.%u, native window realized)\n",
               gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version());
        gtk_widget_destroy(window);
        return 0;
    }
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
