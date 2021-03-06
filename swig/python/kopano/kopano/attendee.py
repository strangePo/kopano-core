# SPDX-License-Identifier: AGPL-3.0-only
"""
Part of the high-level python bindings for Kopano

Copyright 2018 - 2019 Kopano and its licensors (see LICENSE file)
"""

from MAPI.Tags import (
    PR_ADDRTYPE_W, PR_DISPLAY_NAME_W, PR_EMAIL_ADDRESS_W, PR_ENTRYID,
    PR_SEARCH_KEY, PR_RECIPIENT_TRACKSTATUS, PR_RECIPIENT_TRACKSTATUS_TIME,
    PR_RECIPIENT_TYPE, respNone, respTentative, respAccepted, respDeclined,
    respOrganized, respNotResponded,
)
from MAPI import MAPI_TO, MAPI_CC, MAPI_BCC

from .address import Address

PROPS = (PR_ADDRTYPE_W, PR_DISPLAY_NAME_W, PR_EMAIL_ADDRESS_W,
         PR_ENTRYID, PR_SEARCH_KEY)

class Attendee(object):
    """Attendee class

    Abstraction for :class:`appointment <Appointment>` attendees.
    """

    def __init__(self, server, mapirow):
        self.server = server
        self._mapirow = mapirow
        self._row = dict([(x.proptag, x) for x in mapirow])

    @property
    def address(self):
        """Attendee :class:`address <Address>`."""
        args = [self._row[p].value if p in self._row else None for p in PROPS]
        return Address(self.server, *args, props=self._mapirow)

    @property
    def response(self):
        """Attendee response status (no_response, accepted, declined,
        tentatively_accepted, organizer).
        """
        prop = self._row.get(PR_RECIPIENT_TRACKSTATUS)
        if prop:
            return {
                respNone: 'None',
                respOrganized: 'Organizer',
                respTentative: 'TentativelyAccepted',
                respAccepted: 'Accepted',
                respDeclined: 'Declined',
                respNotResponded: 'NotResponded',
            }.get(prop.value)

    @property
    def response_time(self):
        """Attendee response time."""
        prop = self._row.get(PR_RECIPIENT_TRACKSTATUS_TIME)
        if prop:
            return prop.value

    @property
    def type_(self):
        """Attendee type (required, optional or resource)"""
        # TODO is it just webapp which uses this?
        # (as there are explicit meeting properties for this)
        prop = self._row.get(PR_RECIPIENT_TYPE)
        if prop:
            return {
                MAPI_TO: 'required',
                MAPI_CC: 'optional',
                MAPI_BCC: 'resource',
            }.get(prop.value)

    def __unicode__(self):
        return 'Attendee({})'.format(self.address.name)

    def __repr__(self):
        return self.__unicode__()
