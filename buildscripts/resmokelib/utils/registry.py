"""Utility for having class declarations.

The registry automatically causes a reference to the class to be stored along with its name.

This pattern enables the associated class to be looked up later by using
its name.
"""

from buildscripts.resmokelib.utils import default_if_none

# Specifying 'LEAVE_UNREGISTERED' as the "REGISTERED_NAME" attribute will cause the class to be
# omitted from the registry. This is particularly useful for base classes that define an interface
# or common functionality, and aren't intended to be constructed explicitly.
LEAVE_UNREGISTERED = object()


def make_registry_metaclass(registry_store, base_metaclass=None):
    """Return a new Registry metaclass."""

    if not isinstance(registry_store, dict):
        raise TypeError("'registry_store' argument must be a dict")

    base_metaclass = default_if_none(base_metaclass, type)

    class Registry(base_metaclass):
        """A metaclass that stores a reference to all registered classes."""

        def __new__(mcs, class_name, base_classes, class_dict):  # pylint: disable=bad-mcs-classmethod-argument,bad-classmethod-argument
            """Create and returns a new instance of Registry.

            The registry is a class named 'class_name' derived from 'base_classes'
            that defines 'class_dict' as additional attributes.

            The returned class is added to 'registry_store' using
            class_dict["REGISTERED_NAME"] as the name, or 'class_name'
            if the "REGISTERED_NAME" attribute isn't defined. If the
            sentinel value 'LEAVE_UNREGISTERED' is specified as the
            name, then the returned class isn't added to
            'registry_store'.

            The returned class will have the "REGISTERED_NAME" attribute
            defined either as its associated key in 'registry_store' or
            the 'LEAVE_UNREGISTERED' sentinel value.
            """

            registered_name = class_dict.setdefault("REGISTERED_NAME", class_name)
            cls = base_metaclass.__new__(mcs, class_name, base_classes, class_dict)

            if registered_name is not LEAVE_UNREGISTERED:
                if registered_name in registry_store:
                    raise ValueError(
                        "The name %s is already registered; a different value for the"
                        " 'REGISTERED_NAME' attribute must be chosen" % (registered_name))
                registry_store[registered_name] = cls

            return cls

    return Registry
