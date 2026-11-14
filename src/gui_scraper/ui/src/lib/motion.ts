import type { Transition, Variants } from "motion/react";

export const EASE_CINEMA = [0.22, 1, 0.36, 1] as const;
export const EASE_OUT_EXPO = [0.16, 1, 0.3, 1] as const;

export const PILL_SPRING: Transition = {
  type: "spring",
  stiffness: 440,
  damping: 34,
};

export const CARD_SPRING: Transition = {
  type: "spring",
  stiffness: 380,
  damping: 28,
};

export const PAGE_VARIANTS: Variants = {
  initial: { opacity: 0, y: 10 },
  animate: {
    opacity: 1,
    y: 0,
    transition: { duration: 0.28, ease: EASE_CINEMA },
  },
  exit: {
    opacity: 0,
    y: -6,
    transition: { duration: 0.16, ease: "easeIn" },
  },
};

export const HERO_VARIANTS: Variants = {
  initial: { opacity: 0, scale: 0.99 },
  animate: {
    opacity: 1,
    scale: 1,
    transition: { duration: 0.38, ease: EASE_CINEMA },
  },
  exit: {
    opacity: 0,
    transition: { duration: 0.22 },
  },
};

export const CARD_LIST_VARIANTS: Variants = {
  hidden: { opacity: 0, y: 12 },
  visible: (index: number) => ({
    opacity: 1,
    y: 0,
    transition: {
      duration: 0.28,
      ease: EASE_OUT_EXPO,
      delay: Math.min(index, 12) * 0.03,
    },
  }),
};

export const TOAST_VARIANTS: Variants = {
  hidden: { opacity: 0, y: -10, scale: 0.96 },
  visible: {
    opacity: 1,
    y: 0,
    scale: 1,
    transition: { duration: 0.22, ease: EASE_OUT_EXPO },
  },
  exit: {
    opacity: 0,
    y: -8,
    scale: 0.96,
    transition: { duration: 0.16, ease: "easeIn" },
  },
};

export const BADGE_SWAP_VARIANTS: Variants = {
  initial: { opacity: 0, scale: 0.94 },
  animate: { opacity: 1, scale: 1, transition: { duration: 0.18, ease: EASE_OUT_EXPO } },
  exit: { opacity: 0, scale: 0.94, transition: { duration: 0.12 } },
};
