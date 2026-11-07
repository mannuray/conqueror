"""Shared distances (AI engine LLD §19.2, review F5): one name each for ideas used in several places."""

SAME_SPOT_M = (
    0.5  # one thing's floor points scatter this much: the memory's merge, the candidates' merge, the
)
# tracker's "no second thing this near a listed one" (design-notes §189, §190 I5)
SURE_REACH_M = 1.0  # the detector names a toy for sure from this near (design-notes §188: 58-68 % of pictures
# under 1 m, 16 % at 1-1.5 m): the search's look
CHECK_CLOSE_M = 0.5  # a maybe is checked from this near, tank centre (the lens 0.42-0.5 m off, the pan on
# it): the camera is only sure that close (owner, design-notes §215; LLD §24 C)
