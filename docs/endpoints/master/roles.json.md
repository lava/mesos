---
title: Apache Mesos - HTTP Endpoints - /roles.json
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /roles.json
>        /master/roles.json

### TL;DR; ###
Information about roles.

### DESCRIPTION ###
This endpoint provides information about roles as a JSON object.
It returns information about every role that is on the role
whitelist (if enabled), has one or more registered frameworks,
or has a non-default weight or quota. For each role, it returns
the weight, total allocated resources, and registered frameworks.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.