#include <mgos.h>
#include <mgos_homeassistant.h>
#include <mgos_timers.h>

#include <mgos-helpers/log.h>
#include <mgos-helpers/mem.h>

#include <atc_mi.h>

struct atc_mi_ha {
  struct atc_mi_data amd;
  int rssi;
  bool relayed : 1;
  bool rts : 1;
  bool stalled : 1;
};

static const struct mgos_config_atc_mi_ha *cfg = NULL;

static struct atc_mi_ha *ha_amh(struct mgos_homeassistant_object *o,
                                struct json_out *out) {
  if (!o) FNERR_RET(NULL, "NULL %s", "o");
  if (!o->user_data) FNERR_RET(NULL, "NULL %s", "o->user_data");
  if (!out) FNERR_RET(NULL, "NULL %s", "out");
  return o->user_data;
}

static void ha_amh_status(struct mgos_homeassistant_object *o,
                          struct json_out *out) {
  struct atc_mi_ha *amh = ha_amh(o, out);
  if (!amh) return;
  json_printf(out, "rssi:%d", amh->rssi);
  if (amh->relayed) json_printf(out, ",%Q:%Q", "relayed", "yes");
  if (cfg->status.flags && amh->amd.flags != ATC_MI_DATA_FLAGS_INVAL)
    json_printf(out, ",%Q:%u", "flags", amh->amd.flags);
  if (cfg->status.counter) json_printf(out, ",%Q:%u", "counter", amh->amd.cnt);
}

#define HA_AMH_STAT(class, fmt, inval, attr, extra)                \
  static void ha_amh_##class(struct mgos_homeassistant_object * o, \
                             struct json_out * out) {              \
    struct atc_mi_ha *amh = ha_amh(o, out);                        \
    if (amh && amh->amd.attr != inval)                             \
      json_printf(out, fmt, amh->amd.attr extra);                  \
  }
HA_AMH_STAT(battery, "%u", ATC_MI_DATA_BATT_PCT_INVAL, batt_pct, )
HA_AMH_STAT(voltage, "%u", ATC_MI_DATA_BATT_MV_INVAL, batt_mV, )
HA_AMH_STAT(humidity, "%.2f", ATC_MI_DATA_HUMI_CPCT_INVAL, humi_cPct, / 100.0)
HA_AMH_STAT(temperature, "%.2f", ATC_MI_DATA_TEMP_CC_INVAL, temp_cC, / 100.0)
#undef HA_AMH_STAT

static struct mgos_homeassistant_object *ha_obj_add(
    struct mgos_homeassistant *ha, char *name) {
  struct atc_mi_ha *amh = NULL;
  amh = TRY_CALLOC_OR(goto err, amh);
  amh->amd.batt_mV = ATC_MI_DATA_BATT_MV_INVAL;
  amh->amd.batt_pct = ATC_MI_DATA_BATT_PCT_INVAL;
  amh->amd.flags = ATC_MI_DATA_FLAGS_INVAL;
  amh->amd.humi_cPct = ATC_MI_DATA_HUMI_CPCT_INVAL;
  amh->amd.temp_cC = ATC_MI_DATA_TEMP_CC_INVAL;
  amh->stalled = true;

  struct mgos_homeassistant_object *o = mgos_homeassistant_object_add(
      ha, name, COMPONENT_SENSOR, NULL, ha_amh_status, amh);
  if (!o) FNERR_GT("failed to add HA object %s", name);

#define HA_CLASS(o, name, class, unit)                                    \
  do {                                                                    \
    if (cfg->status.class &&                                              \
        !mgos_homeassistant_object_class_add(                             \
            o, #class,                                                    \
            "\"unit_of_meas\":\"" unit "\",\"stat_cla\":\"measurement\"", \
            ha_amh_##class))                                              \
      FNERR_GT("failed to add %s class to HA object %s", #class, name);   \
  } while (0)
  HA_CLASS(o, name, battery, "%");
  HA_CLASS(o, name, voltage, "mV");
  HA_CLASS(o, name, humidity, "%");
  HA_CLASS(o, name, temperature, "°C");
#undef HA_CLASS

  FNLOG(LL_INFO, "added HA object %s", name);
  return o;

err:
  if (o) mgos_homeassistant_object_remove(&o);
  if (amh) free(amh);
  return NULL;
}

static char *ha_obj_get_name(char *buf, size_t bufL, const uint8_t mac[6],
                             const struct atc_mi *am) {
  if (am && am->name) {
    snprintf(buf, bufL, "%s%s", cfg->names.prefix, am->name);
    return buf;
  }

  const char *fmt = "%s%5$02X%6$02X%7$02X";
  if (cfg->names.full_mac) fmt = "%s%02X%02X%02X%02X%02X%02X";
  snprintf(buf, bufL, fmt, cfg->names.prefix, mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);
  return buf;
}

static struct mgos_homeassistant_object *ha_obj_get_or_add(
    struct mgos_homeassistant *ha, const uint8_t mac[6], struct atc_mi *am) {
  if (am) return am->user_data;
  char *name = ha_obj_get_name(alloca(32), 32, mac, NULL);
  struct mgos_homeassistant_object *o = mgos_homeassistant_object_get(ha, name);
  return !o ? ha_obj_add(ha, name) : strcmp(o->object_name, name) ? NULL : o;
}

static bool amh_obj_fromjson(struct mgos_homeassistant *ha,
                             struct json_token v) {
  struct atc_mi *am = atc_mi_load_json(v);
  if (!am) return false;
  am->user_data = ha_obj_add(ha, ha_obj_get_name(alloca(32), 32, am->mac, am));
  if (am->user_data && atc_mi_add(am)) return true;
  if (am->user_data) mgos_homeassistant_object_remove((void *) &am->user_data);
  atc_mi_free(am);
  return false;
}

static void amh_timer(void *opaque) {
  struct mgos_homeassistant_object *o = opaque;
  struct atc_mi_ha *amh = o->user_data;
  if (!amh->rts)
    amh->stalled = true;
  else {
    mgos_homeassistant_object_send_status(o);
    amh->amd.flags = ATC_MI_DATA_FLAGS_INVAL;
    amh->rts = false;
    amh->stalled = mgos_set_timer(cfg->min_period * 1000, 0, amh_timer, o) ==
                   MGOS_INVALID_TIMER_ID;
  }
}

static void am_sink(int ev, void *ev_data, void *userdata) {
  if (ev != ATC_MI_EVENT_DATA) return;

  struct atc_mi_event_data *amed = ev_data;
  const struct atc_mi_data *amd = amed->data;
  struct mgos_homeassistant_object *o =
      ha_obj_get_or_add(userdata, amd->mac, amed->atc_mi);
  if (!o) return;

  struct atc_mi_ha *amh = o->user_data;
  bool relayed = !!memcmp(amed->res->addr.addr, amd->mac, sizeof(amd->mac));
  if (amh->rts && amh->amd.cnt == amd->cnt && relayed) return;  // Relayed dupe?
  amh->relayed = relayed;
  amh->rssi = amed->res->rssi;

#define HA_SINK_STAT(inval, attr, class)                                    \
  do {                                                                      \
    if (cfg->status.class && amd->attr != inval) amh->amd.attr = amd->attr; \
  } while (0)
  HA_SINK_STAT(ATC_MI_DATA_BATT_MV_INVAL, batt_mV, voltage);
  HA_SINK_STAT(ATC_MI_DATA_BATT_PCT_INVAL, batt_pct, battery);
  HA_SINK_STAT(ATC_MI_DATA_HUMI_CPCT_INVAL, humi_cPct, humidity);
  HA_SINK_STAT(ATC_MI_DATA_TEMP_CC_INVAL, temp_cC, temperature);
#undef HA_SINK_STAT
  if (amd->flags != ATC_MI_DATA_FLAGS_INVAL) {
    if (amh->amd.flags == ATC_MI_DATA_FLAGS_INVAL)
      amh->amd.flags = amd->flags;
    else
      amh->amd.flags |= amd->flags;
  }
  amh->amd.cnt = amd->cnt;

  amh->rts = true;
  if (amh->stalled)
    amh->stalled = mgos_set_timer(0, MGOS_TIMER_RUN_NOW, amh_timer, o) ==
                   MGOS_INVALID_TIMER_ID;
}

bool mgos_atc_mi_ha_init(void) {
  cfg = mgos_config_get_atc_mi_ha(&mgos_sys_config);
  if (!cfg->enable) return true;
  TRY_RETF(mgos_homeassistant_register_provider, "atc_mi", amh_obj_fromjson,
           NULL);
  TRY_RETF(mgos_event_add_handler, ATC_MI_EVENT_DATA, am_sink,
           mgos_homeassistant_get_global());
  return true;
}
