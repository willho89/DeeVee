#include "deevee_nav.h"

#include <string.h>

void deevee_nav_init(struct deevee_nav *nav)
{
   if (!nav)
      return;

   memset(nav, 0, sizeof(*nav));
}

void deevee_nav_set_button(struct deevee_nav *nav,
      enum deevee_nav_button button, bool pressed)
{
   if (!nav || button < 0 || button >= DEEVEE_NAV_BUTTON_COUNT)
      return;

   nav->buttons[button] = pressed;
}

uint32_t deevee_nav_active_mask(const struct deevee_nav *nav)
{
   uint32_t mask = 0;
   int i;

   if (!nav)
      return 0;

   for (i = 0; i < DEEVEE_NAV_BUTTON_COUNT; i++)
      if (nav->buttons[i])
         mask |= (uint32_t)1 << i;

   return mask;
}
