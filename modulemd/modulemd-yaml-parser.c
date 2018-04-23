/* modulemd-yaml-parser.c
 *
 * Copyright (C) 2017 Stephen Gallagher
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE X CONSORTIUM BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <inttypes.h>
#include <yaml.h>
#include <errno.h>
#include "modulemd.h"
#include "modulemd-yaml.h"
#include "modulemd-util.h"
#include "modulemd-subdocument-private.h"

GQuark
modulemd_yaml_error_quark (void)
{
  return g_quark_from_static_string ("modulemd-yaml-error-quark");
}


static gboolean
_parse_yaml (yaml_parser_t *parser,
             GPtrArray **data,
             GPtrArray **failures,
             GError **error);

static gboolean
_read_yaml_and_type (yaml_parser_t *parser, ModulemdSubdocument **subdocument);

static gboolean
_parse_subdocument (ModulemdSubdocument *subdocument,
                    ModulemdParsingFunc parse_func,
                    GObject **data,
                    guint64 version,
                    GError **error);


gboolean
parse_yaml_file (const gchar *path,
                 GPtrArray **data,
                 GPtrArray **failures,
                 GError **error)
{
  gboolean result = FALSE;
  FILE *yaml_file = NULL;
  yaml_parser_t parser;

  g_debug ("TRACE: entering parse_yaml_file");

  if (error != NULL && *error != NULL)
    {
      MMD_ERROR_RETURN_FULL (
        error, MODULEMD_YAML_ERROR_PROGRAMMING, "GError is initialized.");
    }

  if (!path)
    {
      MMD_ERROR_RETURN_FULL (
        error, MODULEMD_YAML_ERROR_PROGRAMMING, "Path not supplied.");
    }

  yaml_parser_initialize (&parser);

  errno = 0;
  yaml_file = g_fopen (path, "rb");
  if (!yaml_file)
    {
      g_set_error (error,
                   MODULEMD_YAML_ERROR,
                   MODULEMD_YAML_ERROR_OPEN,
                   "Failed to open file: %s",
                   g_strerror (errno));
      goto error;
    }

  yaml_parser_set_input_file (&parser, yaml_file);

  if (!_parse_yaml (&parser, data, failures, error))
    {
      MMD_YAML_ERROR_RETURN_RETHROW (error, "Could not parse YAML");
    }

  result = TRUE;

error:
  yaml_parser_delete (&parser);
  if (yaml_file)
    {
      fclose (yaml_file);
    }
  g_debug ("TRACE: exiting parse_yaml_file");
  return result;
}

gboolean
parse_yaml_string (const gchar *yaml,
                   GPtrArray **data,
                   GPtrArray **failures,
                   GError **error)
{
  gboolean result = FALSE;
  yaml_parser_t parser;

  g_debug ("TRACE: entering parse_yaml_string");

  if (error != NULL && *error != NULL)
    {
      MMD_ERROR_RETURN_FULL (
        error, MODULEMD_YAML_ERROR_PROGRAMMING, "GError is initialized.");
    }

  if (!yaml)
    {
      MMD_ERROR_RETURN_FULL (
        error, MODULEMD_YAML_ERROR_PROGRAMMING, "String not supplied.");
    }

  yaml_parser_initialize (&parser);

  yaml_parser_set_input_string (
    &parser, (const unsigned char *)yaml, strlen (yaml));

  if (!_parse_yaml (&parser, data, failures, error))
    {
      MMD_YAML_ERROR_RETURN_RETHROW (error, "Could not parse YAML");
    }

  result = TRUE;

error:
  yaml_parser_delete (&parser);

  g_debug ("TRACE: exiting parse_yaml_string");
  return result;
}


gboolean
parse_yaml_stream (FILE *stream,
                   GPtrArray **data,
                   GPtrArray **failures,
                   GError **error)
{
  gboolean result = FALSE;
  yaml_parser_t parser;

  g_debug ("TRACE: entering parse_yaml_stream");

  if (error != NULL && *error != NULL)
    {
      MMD_ERROR_RETURN_FULL (
        error, MODULEMD_YAML_ERROR_PROGRAMMING, "GError is initialized.");
    }

  if (!stream)
    {
      MMD_ERROR_RETURN_FULL (
        error, MODULEMD_YAML_ERROR_PROGRAMMING, "Stream not supplied.");
    }

  yaml_parser_initialize (&parser);

  yaml_parser_set_input_file (&parser, stream);

  if (!_parse_yaml (&parser, data, failures, error))
    {
      MMD_YAML_ERROR_RETURN_RETHROW (error, "Could not parse YAML");
    }

  result = TRUE;

error:
  yaml_parser_delete (&parser);
  g_debug ("TRACE: exiting parse_yaml_stream");
  return result;
}


static gboolean
_parse_yaml (yaml_parser_t *parser,
             GPtrArray **data,
             GPtrArray **failures,
             GError **error)
{
  gboolean result = FALSE;
  gboolean done = FALSE;
  yaml_event_t event;
  g_autoptr (GPtrArray) subdocuments = NULL;
  g_autoptr (GPtrArray) failed_subdocuments = NULL;
  g_autoptr (GPtrArray) objects = NULL;
  g_autoptr (ModulemdSubdocument) document = NULL;
  ModulemdSubdocument *subdocument = NULL;
  g_autoptr (GError) subdocument_error = NULL;

  GObject *object = NULL;

  g_debug ("TRACE: entering _parse_yaml");

  /* Read through the complete stream once, separating subdocuments and
   * identifying their types
   */
  subdocuments = g_ptr_array_new_full (1, g_object_unref);
  failed_subdocuments = g_ptr_array_new_with_free_func (g_object_unref);
  objects = g_ptr_array_new_full (1, g_object_unref);

  while (!done)
    {
      YAML_PARSER_PARSE_WITH_ERROR_RETURN (
        parser, &event, error, "Parser error");

      switch (event.type)
        {
        case YAML_STREAM_START_EVENT:
          /* The beginning of the YAML stream */
          break;

        case YAML_STREAM_END_EVENT:
          /* All of the subdocuments have been processed */
          done = TRUE;
          break;

        case YAML_DOCUMENT_START_EVENT:
          if (!_read_yaml_and_type (parser, &document))
            {
              g_ptr_array_add (failed_subdocuments, g_object_ref (document));

              if (error)
                {
                  *error =
                    g_error_copy (modulemd_subdocument_get_gerror (document));
                }
              MMD_YAML_ERROR_RETURN_RETHROW (
                error, "Parse error during preprocessing");
            }

          /* Add all valid documents to the list */
          if (modulemd_subdocument_get_doctype (document) != G_TYPE_INVALID)
            {
              g_ptr_array_add (subdocuments, g_object_ref (document));
            }
          else
            {
              /* Any documents we're skipping should also go into this list */
              g_ptr_array_add (failed_subdocuments, g_object_ref (document));
            }

          g_clear_pointer (&document, g_object_unref);
          break;

        default:
          /* We received a YAML event we shouldn't expect at this level */
          MMD_YAML_ERROR_RETURN_RETHROW (
            error, "Unexpected YAML event during preprocessing");
          break;
        }

      yaml_event_delete (&event);
    }

  /* Iterate through the subdocuments and process them by type */
  for (gsize i = 0; i < subdocuments->len; i++)
    {
      subdocument = g_ptr_array_index (subdocuments, i);
      if (modulemd_subdocument_get_doctype (subdocument) ==
          MODULEMD_TYPE_MODULE)
        {
          result =
            _parse_subdocument (subdocument,
                                _parse_modulemd,
                                &object,
                                modulemd_subdocument_get_version (subdocument),
                                &subdocument_error);
        }
      /* Parsers for other types go here */
      else if (modulemd_subdocument_get_doctype (subdocument) ==
               MODULEMD_TYPE_DEFAULTS)
        {
          result =
            _parse_subdocument (subdocument,
                                _parse_defaults,
                                &object,
                                modulemd_subdocument_get_version (subdocument),
                                &subdocument_error);
        }
      /* else if (document->type == <...>) */
      else
        {
          /* Unknown document type */
          g_set_error_literal (error,
                               MODULEMD_YAML_ERROR,
                               MODULEMD_YAML_ERROR_PARSE,
                               "Unknown document type");
          result = FALSE;
        }

      if (result)
        {
          g_ptr_array_add (objects, object);
        }
      else
        {
          modulemd_subdocument_set_gerror (subdocument, subdocument_error);
          g_clear_error (&subdocument_error);

          g_ptr_array_add (failed_subdocuments, g_object_ref (subdocument));

          g_debug ("Skipping invalid document");
          g_clear_error (error);
        }
    }

  if (data)
    {
      *data = g_ptr_array_ref (objects);
    }

  result = TRUE;

error:
  if (failures)
    {
      *failures = g_ptr_array_ref (failed_subdocuments);
    }

  return result;
}


static gboolean
_read_yaml_and_type (yaml_parser_t *parser, ModulemdSubdocument **subdocument)
{
  g_autoptr (ModulemdSubdocument) document = NULL;
  g_autoptr (GError) error = NULL;
  gboolean result = FALSE;
  gboolean done = FALSE;
  gboolean finish_invalid_document = FALSE;
  gsize depth = 0;
  g_autoptr (modulemd_yaml_string) yaml_string = NULL;
  yaml_event_t event;
  yaml_event_t value_event;
  yaml_emitter_t emitter;

  g_debug ("TRACE: entering _read_yaml_and_type");

  document = modulemd_subdocument_new ();

  yaml_string = g_malloc0_n (1, sizeof (modulemd_yaml_string));
  yaml_emitter_initialize (&emitter);

  yaml_emitter_set_output (&emitter, _write_yaml_string, (void *)yaml_string);

  yaml_stream_start_event_initialize (&event, YAML_UTF8_ENCODING);
  YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
    &emitter, &event, &error, "Error starting stream");

  yaml_document_start_event_initialize (&event, NULL, NULL, NULL, 0);
  YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
    &emitter, &event, &error, "Error starting document");


  while (!done)
    {
      value_event.type = YAML_NO_EVENT;
      YAML_PARSER_PARSE_WITH_ERROR_RETURN (
        parser, &event, &error, "Parser error");

      switch (event.type)
        {
        case YAML_DOCUMENT_END_EVENT: done = TRUE; break;

        case YAML_SEQUENCE_START_EVENT:
        case YAML_MAPPING_START_EVENT: depth++; break;

        case YAML_SEQUENCE_END_EVENT:
        case YAML_MAPPING_END_EVENT: depth--; break;

        case YAML_SCALAR_EVENT:
          if (depth == 1 && !finish_invalid_document)
            {
              /* If we're in the root of the document, check for the
               * document type and version
               */

              if (!g_strcmp0 ((const gchar *)event.data.scalar.value,
                              "document"))
                {
                  if (modulemd_subdocument_get_doctype (document) !=
                      G_TYPE_INVALID)
                    {
                      /* We encountered document-type twice in the same
                       * document root mapping. This shouldn't ever happen
                       */
                      g_debug ("Document type specified more than once");
                      modulemd_subdocument_set_doctype (document,
                                                        G_TYPE_INVALID);

                      /*
                       * This is a recoverable parsing error, so we don't want
                       * to exit with FALSE.
                       */
                      finish_invalid_document = TRUE;

                      g_set_error (
                        &error,
                        MODULEMD_YAML_ERROR,
                        MODULEMD_YAML_ERROR_PARSE,
                        "Document type was specified more than once");

                      break;
                    }

                  YAML_PARSER_PARSE_WITH_ERROR_RETURN (
                    parser, &value_event, &error, "Parser error");

                  if (value_event.type != YAML_SCALAR_EVENT)
                    {
                      g_debug ("Document type not a scalar");
                      modulemd_subdocument_set_doctype (document,
                                                        G_TYPE_INVALID);

                      switch (value_event.type)
                        {
                        case YAML_SEQUENCE_START_EVENT:
                        case YAML_MAPPING_START_EVENT: depth++; break;

                        case YAML_SEQUENCE_END_EVENT:
                        case YAML_MAPPING_END_EVENT: depth--; break;

                        default: break;
                        }

                      /*
                       * This is a recoverable parsing error, so we don't want
                       * to exit with FALSE.
                       */
                      finish_invalid_document = TRUE;

                      g_set_error (&error,
                                   MODULEMD_YAML_ERROR,
                                   MODULEMD_YAML_ERROR_PARSE,
                                   "Document type was not a scalar value");

                      break;
                    }

                  if (g_strcmp0 ((const gchar *)value_event.data.scalar.value,
                                 "modulemd") == 0)
                    {
                      modulemd_subdocument_set_doctype (document,
                                                        MODULEMD_TYPE_MODULE);
                    }

                  else if (g_strcmp0 (
                             (const gchar *)value_event.data.scalar.value,
                             "modulemd-defaults") == 0)
                    {
                      modulemd_subdocument_set_doctype (
                        document, MODULEMD_TYPE_DEFAULTS);
                    }
                  /* Handle additional types here */

                  else
                    {
                      /* Unknown document type */
                      modulemd_subdocument_set_doctype (document,
                                                        G_TYPE_INVALID);

                      finish_invalid_document = TRUE;

                      g_set_error (&error,
                                   MODULEMD_YAML_ERROR,
                                   MODULEMD_YAML_ERROR_PARSE,
                                   "Document type is not recognized");
                    }

                  g_debug (
                    "Document type: %s",
                    g_type_name (modulemd_subdocument_get_doctype (document)));
                }

              else if (g_strcmp0 ((const gchar *)event.data.scalar.value,
                                  "version") == 0)
                {
                  if (modulemd_subdocument_get_version (document) != 0)
                    {
                      g_debug ("Document version specified more than once");
                      modulemd_subdocument_set_doctype (document,
                                                        G_TYPE_INVALID);

                      /*
                       * This is a recoverable parsing error, so we don't want
                       * to exit with FALSE.
                       */
                      finish_invalid_document = TRUE;

                      g_set_error (
                        &error,
                        MODULEMD_YAML_ERROR,
                        MODULEMD_YAML_ERROR_PARSE,
                        "Document version was specified more than once");

                      break;
                    }

                  YAML_PARSER_PARSE_WITH_ERROR_RETURN (
                    parser, &value_event, &error, "Parser error");

                  if (value_event.type != YAML_SCALAR_EVENT)
                    {
                      g_debug ("Document version not a scalar");
                      modulemd_subdocument_set_doctype (document,
                                                        G_TYPE_INVALID);

                      /*
                       * This is a recoverable parsing error, so we don't want
                       * to exit with FALSE.
                       */
                      finish_invalid_document = TRUE;

                      g_set_error (&error,
                                   MODULEMD_YAML_ERROR,
                                   MODULEMD_YAML_ERROR_PARSE,
                                   "Document version was not a scalar");

                      switch (value_event.type)
                        {
                        case YAML_SEQUENCE_START_EVENT:
                        case YAML_MAPPING_START_EVENT: depth++; break;

                        case YAML_SEQUENCE_END_EVENT:
                        case YAML_MAPPING_END_EVENT: depth--; break;

                        default: break;
                        }

                      break;
                    }
                  modulemd_subdocument_set_version (
                    document,
                    g_ascii_strtoull (
                      (const gchar *)value_event.data.scalar.value, NULL, 10));

                  g_debug ("Document version: %" PRIx64,
                           modulemd_subdocument_get_version (document));
                }
            }
          break;

        default:
          /* Just fall through here. */
          break;
        }

      /* Copy this event to the string */
      YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
        &emitter, &event, &error, "Error storing YAML event");

      if (value_event.type != YAML_NO_EVENT)
        {
          /* Copy this event to the string */
          YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
            &emitter, &value_event, &error, "Error storing YAML event");
        }
    }

  yaml_stream_end_event_initialize (&event);
  YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
    &emitter, &event, &error, "Error ending stream");

  yaml_event_delete (&event);

  /* If we get here with an invalid document type and no error */
  if (modulemd_subdocument_get_doctype (document) == G_TYPE_INVALID &&
      error == NULL)
    {
      g_set_error (&error,
                   MODULEMD_YAML_ERROR,
                   MODULEMD_YAML_ERROR_PARSE,
                   "Document type was unspecified or unknown");
    }

  result = TRUE;
error:
  yaml_emitter_delete (&emitter);

  modulemd_subdocument_set_gerror (document, error);

  /* Copy the string, even if it was only partial because it's still useful
   * to know where parsing broke
   */
  modulemd_subdocument_set_yaml (document, yaml_string->str);
  if (subdocument)
    *subdocument = g_object_ref (document);

  g_debug ("TRACE: exiting _read_yaml_and_type");
  return result;
}

static gboolean
_parse_subdocument (ModulemdSubdocument *subdocument,
                    ModulemdParsingFunc parse_func,
                    GObject **data,
                    guint64 version,
                    GError **error)
{
  gboolean result = FALSE;
  yaml_event_t event;
  gboolean done = FALSE;
  GObject *object = NULL;
  yaml_parser_t parser;

  yaml_parser_initialize (&parser);
  yaml_parser_set_input_string (
    &parser,
    (const unsigned char *)modulemd_subdocument_get_yaml (subdocument),
    strlen (modulemd_subdocument_get_yaml (subdocument)));

  while (!done)
    {
      YAML_PARSER_PARSE_WITH_ERROR_RETURN (
        &parser, &event, error, "Parser error");

      switch (event.type)
        {
        case YAML_STREAM_START_EVENT:
          /* Starting the stream here */
          break;

        case YAML_DOCUMENT_START_EVENT:
          if (!parse_func (&parser, &object, version, error))
            {
              goto error;
            }
          break;

        case YAML_DOCUMENT_END_EVENT:
          /* This document is complete. */
          break;

        case YAML_STREAM_END_EVENT: done = TRUE; break;

        default:
          /* We received a YAML event we shouldn't expect at this level */
          MMD_YAML_ERROR_RETURN (error, "Unexpected YAML event at toplevel");
          break;
        }

      yaml_event_delete (&event);
    }

  *data = object;
  result = TRUE;

error:
  yaml_parser_delete (&parser);
  g_debug ("TRACE: exiting _parse_yaml");
  return result;
}


gboolean
_parse_modulemd_date (yaml_parser_t *parser, GDate **_date, GError **error)
{
  gboolean result = FALSE;
  gchar **strv = NULL;
  yaml_event_t event;

  YAML_PARSER_PARSE_WITH_ERROR_RETURN (parser, &event, error, "Parser error");
  if (event.type != YAML_SCALAR_EVENT)
    {
      MMD_YAML_ERROR_RETURN (error, "Failed to parse date");
    }

  strv = g_strsplit ((const gchar *)event.data.scalar.value, "-", 4);

  if (!strv[0] || !strv[1] || !strv[2])
    {
      MMD_YAML_ERROR_RETURN (error, "Date not in the form YYYY-MM-DD");
    }

  *_date = g_date_new_dmy (g_ascii_strtoull (strv[2], NULL, 10), /* Day */
                           g_ascii_strtoull (strv[1], NULL, 10), /* Month */
                           g_ascii_strtoull (strv[0], NULL, 10)); /* Year */

  result = TRUE;

error:
  g_clear_pointer (&strv, g_strfreev);
  yaml_event_delete (&event);
  return result;
}

gboolean
_simpleset_from_sequence (yaml_parser_t *parser,
                          ModulemdSimpleSet **_set,
                          GError **error)
{
  gboolean result = FALSE;
  yaml_event_t event;
  gboolean started = FALSE;
  gboolean done = FALSE;
  ModulemdSimpleSet *set = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  g_debug ("TRACE: entering _simpleset_from_sequence");

  set = modulemd_simpleset_new ();


  while (!done)
    {
      YAML_PARSER_PARSE_WITH_ERROR_RETURN (
        parser, &event, error, "Parser error");

      switch (event.type)
        {
        case YAML_SEQUENCE_START_EVENT:
          /* Sequence has begun */
          started = TRUE;
          break;

        case YAML_SEQUENCE_END_EVENT:
          /* Sequence has concluded. Return */
          done = TRUE;
          break;

        case YAML_SCALAR_EVENT:
          if (!started)
            {
              MMD_YAML_ERROR_RETURN (
                error, "Received scalar where sequence expected");
            }
          modulemd_simpleset_add (set, (const gchar *)event.data.scalar.value);
          break;

        default:
          /* We received a YAML event we shouldn't expect at this level */
          MMD_YAML_ERROR_RETURN (error, "Unexpected YAML event in sequence");
          break;
        }
      yaml_event_delete (&event);
    }

  *_set = g_object_ref (set);
  result = TRUE;

error:
  yaml_event_delete (&event);
  g_object_unref (set);

  g_debug ("TRACE: exiting _simpleset_from_sequence");
  return result;
}

gboolean
_hashtable_from_mapping (yaml_parser_t *parser,
                         GHashTable **_htable,
                         GError **error)
{
  gboolean result = FALSE;
  yaml_event_t event;
  yaml_event_t value_event;
  gboolean started = FALSE;
  gboolean done = FALSE;
  GHashTable *htable = NULL;
  gchar *name = NULL;
  gchar *value = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_debug ("TRACE: entering _hashtable_from_mapping");

  htable = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  while (!done)
    {
      YAML_PARSER_PARSE_WITH_ERROR_RETURN (
        parser, &event, error, "Parser error");

      switch (event.type)
        {
        case YAML_MAPPING_START_EVENT:
          /* The dictionary has begun */
          started = TRUE;
          break;

        case YAML_MAPPING_END_EVENT:
          /* We've processed the whole dictionary */
          done = TRUE;
          break;

        case YAML_SCALAR_EVENT:
          if (!started)
            {
              MMD_YAML_ERROR_RETURN (error,
                                     "Received scalar where mapping expected");
            }
          name = g_strdup ((const gchar *)event.data.scalar.value);
          YAML_PARSER_PARSE_WITH_ERROR_RETURN (
            parser, &value_event, error, "Parser error");
          if (value_event.type != YAML_SCALAR_EVENT)
            {
              g_free (name);
              MMD_YAML_ERROR_RETURN (error,
                                     "Non-scalar value for dictionary.");
            }
          value = g_strdup ((const gchar *)value_event.data.scalar.value);
          yaml_event_delete (&value_event);

          /* Set this key and value to the hash table */
          g_hash_table_insert (htable, name, value);

          break;


        default:
          /* We received a YAML event we shouldn't expect at this level */
          MMD_YAML_ERROR_RETURN (error, "Unexpected YAML event in sequence");
          break;
        }

      yaml_event_delete (&event);
    }
  *_htable = g_hash_table_ref (htable);

  result = TRUE;

error:
  yaml_event_delete (&value_event);
  yaml_event_delete (&event);
  g_hash_table_unref (htable);

  g_debug ("TRACE: exiting _hashtable_from_mapping");
  return result;
}

/* Helper function to skip over sections that aren't yet implemented */
gboolean
_parse_skip (yaml_parser_t *parser, GError **error)
{
  yaml_event_t event;
  gboolean result = FALSE;
  gboolean done = FALSE;
  gsize depth = 0;

  while (!done)
    {
      YAML_PARSER_PARSE_WITH_ERROR_RETURN (
        parser, &event, error, "Parser error");

      switch (event.type)
        {
        case YAML_DOCUMENT_END_EVENT: done = TRUE; break;

        case YAML_SEQUENCE_START_EVENT:
        case YAML_MAPPING_START_EVENT: depth++; break;

        case YAML_SEQUENCE_END_EVENT:
        case YAML_MAPPING_END_EVENT:
          depth--;

          if (depth <= 0)
            {
              /* We've come back up to the original level from which we
               * started
               */
              done = TRUE;
            }
          break;

        default:
          /* Just fall through here. */
          break;
        }
      yaml_event_delete (&event);
    }

  result = TRUE;
error:
  yaml_event_delete (&event);
  return result;
}
