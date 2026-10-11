"""Name conversions shared by the validator and the emitters."""


def pascal(name: str) -> str:
    return "".join(part.capitalize() for part in name.lower().split("_"))


def k_name(name: str) -> str:
    return "k" + pascal(name)
