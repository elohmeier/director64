// Shared by reboot and native Activity teardown. Host callbacks and package
// tables must remain valid until every owned runtime object has been released.
static void release_runtime(void) {
  if (!booted) return;
  dg_stop_sounds(&director);
#if DG_D5 || DG_D10
  close_videos();
#if DG_D10
  close_flash_audio();
#endif
#endif
  wc_release(&compositor);
#if DG_D10
  dg_linked_dispose(&director);
  dg_films_dispose(&director);
  dg_images_dispose(&director);
#endif
#if DG_D5
  free(director.suspended_stage);
  director.suspended_stage = NULL;
#endif
  booted = false;
}

void d64_shutdown(void) {
  release_runtime();
  memset(&director, 0, sizeof(director));
  memset(&values, 0, sizeof(values));
  dg_package_free(&package);
  dg_registry = NULL;
  dg_registry_count = global_count = 0;
  global_names = NULL;
  lv_use_symbols((lb_symbol_table_t){0});
  package_error[0] = 0;
  host = (web_host_t){0};
}
