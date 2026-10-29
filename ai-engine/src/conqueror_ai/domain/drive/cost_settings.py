"""The cost map's settings (AI engine LLD §18.13.8, `config/map.yaml` `cost:`): on their own so the map config
can hold them without importing the map."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class CostSettings:
    keep_out_m: float = 0.075  # no point of the body nearer to a known obstacle (about one 5 cm cell)
    near_out_m: float = 0.20  # the fan: near cost from the outline, zero this far out
    near_c_m: float = 0.40  # routes: near cost from the tank's centre, zero this far out
    near_cost: float = 0.8  # the most a near cell costs
    unknown_cost: float = 0.3  # added for a cell never seen
    cost_w: float = 4.0  # the fan's score: this much per unit of the highest cost a move meets
    route_cost_w: float = 6.0  # a route step costs its length x (1 + this x the cell's cost)
    stuck_s: float = 3.0  # no allowed move or no progress this long: recovery
    back_m: float = 0.20  # recovery backs up at most this far
