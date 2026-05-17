#ifndef DEEVEE_NAV_H
#define DEEVEE_NAV_H

#include <stdbool.h>
#include <stdint.h>

enum deevee_nav_button
{
   DEEVEE_NAV_UP = 0,
   DEEVEE_NAV_DOWN,
   DEEVEE_NAV_LEFT,
   DEEVEE_NAV_RIGHT,
   DEEVEE_NAV_CONFIRM,
   DEEVEE_NAV_CANCEL,
   DEEVEE_NAV_MENU,
   DEEVEE_NAV_HOME,
   DEEVEE_NAV_PREVIOUS_CHAPTER,
   DEEVEE_NAV_NEXT_CHAPTER,
   DEEVEE_NAV_BUTTON_COUNT
};

struct deevee_nav
{
   bool buttons[DEEVEE_NAV_BUTTON_COUNT];
};

void deevee_nav_init(struct deevee_nav *nav);
void deevee_nav_set_button(struct deevee_nav *nav,
      enum deevee_nav_button button, bool pressed);
uint32_t deevee_nav_active_mask(const struct deevee_nav *nav);

#endif
