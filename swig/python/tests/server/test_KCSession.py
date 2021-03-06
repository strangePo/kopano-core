from MAPI import kc_session_save, kc_session_restore
from MAPI.Util import GetDefaultStore


def test_save_session(session):
    data = kc_session_save(session)
    assert data


def test_restore_session(session):
    data = kc_session_save(session)
    restored_session = kc_session_restore(data)

    assert restored_session
    # Test if the session works
    assert GetDefaultStore(restored_session)
