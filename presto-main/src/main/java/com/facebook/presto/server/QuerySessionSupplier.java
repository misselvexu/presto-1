/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package com.facebook.presto.server;

import com.facebook.airlift.log.Logger;
import com.facebook.presto.Session;
import com.facebook.presto.common.WarningHandlingLevel;
import com.facebook.presto.common.type.TimeZoneKey;
import com.facebook.presto.execution.warnings.WarningCollectorFactory;
import com.facebook.presto.metadata.SessionPropertyManager;
import com.facebook.presto.security.AccessControl;
import com.facebook.presto.spi.QueryId;
import com.facebook.presto.spi.WarningCollector;
import com.facebook.presto.spi.function.SqlFunctionId;
import com.facebook.presto.spi.function.SqlInvokedFunction;
import com.facebook.presto.spi.security.AuthorizedIdentity;
import com.facebook.presto.spi.security.Identity;
import com.facebook.presto.sql.SqlEnvironmentConfig;
import com.facebook.presto.transaction.TransactionManager;

import javax.annotation.concurrent.ThreadSafe;
import javax.inject.Inject;

import java.util.Locale;
import java.util.Map;
import java.util.Optional;

import static com.facebook.presto.Session.SessionBuilder;
import static com.facebook.presto.SystemSessionProperties.WARNING_HANDLING;
import static com.facebook.presto.common.type.TimeZoneKey.getTimeZoneKey;
import static java.util.Map.Entry;
import static java.util.Objects.requireNonNull;

@ThreadSafe
public class QuerySessionSupplier
        implements SessionSupplier
{
    private final Logger log = Logger.get(QuerySessionSupplier.class);

    private final TransactionManager transactionManager;
    private final AccessControl accessControl;
    private final SessionPropertyManager sessionPropertyManager;
    private final Optional<TimeZoneKey> forcedSessionTimeZone;

    @Inject
    public QuerySessionSupplier(
            TransactionManager transactionManager,
            AccessControl accessControl,
            SessionPropertyManager sessionPropertyManager,
            SqlEnvironmentConfig config)
    {
        this.transactionManager = requireNonNull(transactionManager, "transactionManager is null");
        this.accessControl = requireNonNull(accessControl, "accessControl is null");
        this.sessionPropertyManager = requireNonNull(sessionPropertyManager, "sessionPropertyManager is null");
        requireNonNull(config, "config is null");
        this.forcedSessionTimeZone = requireNonNull(config.getForcedSessionTimeZone(), "forcedSessionTimeZone is null");
    }

    @Override
    public Session createSession(QueryId queryId, SessionContext context, WarningCollectorFactory warningCollectorFactory, Optional<AuthorizedIdentity> authorizedIdentity)
    {
        Identity identity = context.getIdentity();
        if (authorizedIdentity.isPresent()) {
            identity = new Identity(
                    identity.getUser(),
                    identity.getPrincipal(),
                    identity.getRoles(),
                    identity.getExtraCredentials(),
                    identity.getExtraAuthenticators(),
                    Optional.of(authorizedIdentity.get().getUserName()),
                    authorizedIdentity.get().getReasonForSelect());
            log.info(String.format(
                    "For query %s, given user is %s, authorized user is %s",
                    queryId.getId(),
                    identity.getUser(),
                    authorizedIdentity.get().getUserName()));
        }

        SessionBuilder sessionBuilder = Session.builder(sessionPropertyManager)
                .setQueryId(queryId)
                .setIdentity(identity)
                .setSource(context.getSource())
                .setCatalog(context.getCatalog())
                .setSchema(context.getSchema())
                .setRemoteUserAddress(context.getRemoteUserAddress())
                .setUserAgent(context.getUserAgent())
                .setClientInfo(context.getClientInfo())
                .setClientTags(context.getClientTags())
                .setTraceToken(context.getTraceToken())
                .setResourceEstimates(context.getResourceEstimates())
                .setTracer(context.getTracer());

        if (forcedSessionTimeZone.isPresent()) {
            sessionBuilder.setTimeZoneKey(forcedSessionTimeZone.get());
        }
        else if (context.getTimeZoneId() != null) {
            sessionBuilder.setTimeZoneKey(getTimeZoneKey(context.getTimeZoneId()));
        }

        if (context.getLanguage() != null) {
            sessionBuilder.setLocale(Locale.forLanguageTag(context.getLanguage()));
        }

        for (Entry<String, String> entry : context.getSystemProperties().entrySet()) {
            sessionBuilder.setSystemProperty(entry.getKey(), entry.getValue());
        }
        for (Entry<String, Map<String, String>> catalogProperties : context.getCatalogSessionProperties().entrySet()) {
            String catalog = catalogProperties.getKey();
            for (Entry<String, String> entry : catalogProperties.getValue().entrySet()) {
                sessionBuilder.setCatalogSessionProperty(catalog, entry.getKey(), entry.getValue());
            }
        }

        for (Entry<String, String> preparedStatement : context.getPreparedStatements().entrySet()) {
            sessionBuilder.addPreparedStatement(preparedStatement.getKey(), preparedStatement.getValue());
        }

        if (context.supportClientTransaction()) {
            sessionBuilder.setClientTransactionSupport();
        }

        for (Entry<SqlFunctionId, SqlInvokedFunction> entry : context.getSessionFunctions().entrySet()) {
            sessionBuilder.addSessionFunction(entry.getKey(), entry.getValue());
        }

        // Put after setSystemProperty are called
        WarningCollector warningCollector = warningCollectorFactory.create(sessionBuilder.getSystemProperty(WARNING_HANDLING, WarningHandlingLevel.class));
        sessionBuilder.setWarningCollector(warningCollector);

        Session session = sessionBuilder.build();
        if (context.getTransactionId().isPresent()) {
            session = session.beginTransactionId(context.getTransactionId().get(), transactionManager, accessControl);
        }
        return session;
    }
}
