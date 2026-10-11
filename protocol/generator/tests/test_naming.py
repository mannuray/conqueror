from protogen.naming import k_name, pascal


def test_naming() -> None:
    assert pascal("TELEM_FAST") == "TelemFast"
    assert k_name("TELEM_FAST") == "kTelemFast"
